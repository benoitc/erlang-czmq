// Copyright (C) 2012, 2013 Garrett Smith <g@rre.tt>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "czmq.h"
#undef ETERM // collision between zmq.h and erl_interface.h
#include "erl_interface.h"
#include "erl_czmq.h"

ETERM *ETERM_CMD_PATTERN;
ETERM *ETERM_OK;
ETERM *ETERM_UNDEFINED;
ETERM *ETERM_TRUE;
ETERM *ETERM_FALSE;
ETERM *ETERM_TODO;
ETERM *ETERM_PONG;
ETERM *ETERM_ERROR;
ETERM *ETERM_ERROR_INVALID_SOCKET;
ETERM *ETERM_ERROR_BIND_FAILED;
ETERM *ETERM_ERROR_CONNECT_FAILED;

#define SUCCESS 0

#define EXIT_OK 0
#define EXIT_PORT_READ_ERROR 253
#define EXIT_INTERNAL_ERROR 254

#define CMD_BUF_SIZE 10240

#define MAX_SOCKETS 999999

#define assert_tuple_size(term, size) \
    assert(ERL_IS_TUPLE(term)); \
    assert(erl_size(term) == size)

typedef void (*cmd_handler)(ETERM*, erl_czmq_state*);

static int read_exact(byte *buf, int len)
{
    int i, got = 0;

    do {
        if ((i = read(0, buf + got, len - got)) <= 0)
            return i;
        got += i;
    } while (got < len);

    return len;
}

static int read_cmd(int max, byte *buf)
{
    int len;

    if (read_exact(buf, 2) != 2)
        return -1;
    len = (buf[0] << 8) | buf[1];
    if (len > max) {
        fprintf(stderr, "command length (%u) > max buf length (%u)", len, max);
        exit(EXIT_INTERNAL_ERROR);
    }
    return read_exact(buf, len);
}

static int write_exact(byte *buf, int len)
{
    int i, wrote = 0;

    do {
        if ((i = write(1, buf + wrote, len - wrote)) <= 0)
            return (i);
        wrote += i;
    } while (wrote < len);

    return len;
}

static int write_cmd(byte *buf, int len)
{
    byte li;

    li = (len >> 8) & 0xff;
    write_exact(&li, 1);
    li = len & 0xff;
    write_exact(&li, 1);
    return write_exact(buf, len);
}

static int safe_erl_encode(ETERM *term, int buf_size, byte *buf) {
    int term_len, encoded_len;

    if ((term_len = erl_term_len(term)) > buf_size) {
        fprintf(stderr, "term_len %u > buf_size %u", term_len, buf_size);
        exit(EXIT_INTERNAL_ERROR);
    }

    if ((encoded_len = erl_encode(term, buf)) != term_len) {
        fprintf(stderr, "bad result from erl_encode %u, expected %u",
               term_len, encoded_len);
        exit(EXIT_INTERNAL_ERROR);
    }

    return encoded_len;
}

static void write_term(ETERM *term, erl_czmq_state *state) {
    int len = safe_erl_encode(term, ERL_CZMQ_REPLY_BUF_SIZE, state->reply_buf);
    write_cmd(state->reply_buf, len);
}

static void handle_ping(ETERM *args, erl_czmq_state *state) {
    write_term(ETERM_PONG, state);
}

static int save_socket(void *socket, erl_czmq_state *state) {
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (!vector_get(&state->sockets, i)) {
            vector_set(&state->sockets, i, socket);
            return i;
        }
    }
    assert(0);
}

static void handle_zsocket_new(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 1);
    ETERM *type_arg = erl_element(1, args);
    int type = ERL_INT_VALUE(type_arg);

    void *socket = zsocket_new(state->ctx, type);
    assert(socket);

    int index = save_socket(socket, state);
    ETERM *index_term = erl_mk_int(index);
    write_term(index_term, state);
    erl_free_term(index_term);
}

static void *get_socket(int index, erl_czmq_state *state) {
    return vector_get(&state->sockets, index);
}

static void *socket_from_arg(ETERM *args, int arg_pos, erl_czmq_state *state) {
    ETERM *socket_arg = erl_element(arg_pos, args);
    int socket_index = ERL_INT_VALUE(socket_arg);
    return get_socket(socket_index, state);
}

static void handle_zsocket_type_str(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 1);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    char *type_str = zsocket_type_str(socket);
    ETERM *reply = erl_mk_string(type_str);

    write_term(reply, state);

    erl_free_term(reply);
}

static void handle_zsocket_bind(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 2);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    ETERM *endpoint_arg = erl_element(2, args);

    char *endpoint = erl_iolist_to_string(endpoint_arg);
    int rc = zsocket_bind(socket, endpoint);
    if (rc == -1) {
        write_term(ETERM_ERROR_BIND_FAILED, state);
        return;
    }

    ETERM *result = erl_format("{ok,~i}", rc);
    write_term(result, state);

    erl_free(endpoint);
    erl_free_term(result);
}

static void handle_zsocket_connect(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 2);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    ETERM *endpoint_arg = erl_element(2, args);
    char *endpoint = erl_iolist_to_string(endpoint_arg);
    int rc = zsocket_connect(socket, endpoint);
    if (rc == -1) {
        write_term(ETERM_ERROR_CONNECT_FAILED, state);
        return;
    }

    write_term(ETERM_OK, state);

    erl_free(endpoint);
}

static void handle_zsocket_sendmem(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 3);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    ETERM *data_bin_arg = erl_element(2, args);
    void *data_bin = ERL_BIN_PTR(data_bin_arg);
    int data_bin_size = ERL_BIN_SIZE(data_bin_arg);

    ETERM *flags_arg = erl_element(3, args);
    int flags = ERL_INT_VALUE(flags_arg);

    int rc = zsocket_sendmem(socket, data_bin, data_bin_size, flags);
    if (rc == 0) {
        write_term(ETERM_OK, state);
    } else {
        write_term(ETERM_ERROR, state);
    }
}

static void handle_zsocket_destroy(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 1);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    zsocket_destroy(state->ctx, socket);

    write_term(ETERM_OK, state);
}

static void handle_zstr_send(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 2);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    ETERM *data_arg = erl_element(2, args);
    char *data = erl_iolist_to_string(data_arg);
    zstr_send(socket, data);

    write_term(ETERM_OK, state);

    erl_free(data);
}

static void handle_zstr_recv_nowait(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 1);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    char *data = zstr_recv_nowait(socket);

    if (!data) {
        write_term(ETERM_ERROR, state);
        return;
    }

    ETERM *result = erl_format("{ok,~s}", data);
    write_term(result, state);

    erl_free_term(result);
    free(data);
}

static void handle_zframe_recv_nowait(ETERM *args, erl_czmq_state *state) {
    assert_tuple_size(args, 1);

    void *socket = socket_from_arg(args, 1, state);
    if (!socket) {
        write_term(ETERM_ERROR_INVALID_SOCKET, state);
        return;
    }

    zframe_t *frame = zframe_recv_nowait(socket);
    if (!frame) {
        write_term(ETERM_ERROR, state);
        return;
    }

    size_t frame_size = zframe_size(frame);
    byte *frame_data = zframe_data(frame);
    ETERM *data_bin = erl_mk_binary((char*)frame_data, frame_size);

    int more = zframe_more(frame);
    ETERM *more_boolean;
    if (more) {
        more_boolean = ETERM_TRUE;
    } else {
        more_boolean = ETERM_FALSE;
    }

    ETERM *result = erl_format("{ok,{~w,~w}}", data_bin, more_boolean);
    write_term(result, state);

    zframe_destroy(&frame);
    erl_free_term(data_bin);
    erl_free_term(more_boolean);
    erl_free_term(result);
}

static void handle_cmd(byte *buf, erl_czmq_state *state, int handler_count,
                       cmd_handler *handlers) {
    ETERM *cmd_term = erl_decode(buf);
    if (!erl_match(ETERM_CMD_PATTERN, cmd_term)) {
        fprintf(stderr, "invalid cmd format: ");
        erl_print_term(stderr, cmd_term);
        fprintf(stderr, "\n");
        exit(EXIT_INTERNAL_ERROR);
    }

    ETERM *cmd_id_term = erl_element(1, cmd_term);
    int cmd_id = ERL_INT_VALUE(cmd_id_term);
    if (cmd_id < 0 || cmd_id >= handler_count) {
        fprintf(stderr, "cmd_id out of range: %i", cmd_id);
        exit(EXIT_INTERNAL_ERROR);
    }

    ETERM *cmd_args_term = erl_element(2, cmd_term);
    handlers[cmd_id](cmd_args_term, state);

    erl_free_compound(cmd_term);
    erl_free_compound(cmd_id_term);
    erl_free_compound(cmd_args_term);
}

static void init_eterms() {
    ETERM_CMD_PATTERN = erl_format("{_,_}");
    ETERM_OK = erl_mk_atom("ok");
    ETERM_UNDEFINED = erl_mk_atom("undefined");
    ETERM_TRUE = erl_mk_atom("true");
    ETERM_FALSE = erl_mk_atom("false");
    ETERM_TODO = erl_mk_atom("todo");
    ETERM_PONG = erl_mk_atom("pong");
    ETERM_ERROR = erl_mk_atom("error");
    ETERM_ERROR_INVALID_SOCKET = erl_format("{error,invalid_socket}");
    ETERM_ERROR_BIND_FAILED = erl_format("{error,bind_failed}");
    ETERM_ERROR_CONNECT_FAILED = erl_format("{error,connect_failed}");
}

void erl_czmq_init(erl_czmq_state *state) {
    erl_init(NULL, 0);
    init_eterms();
    state->ctx = zctx_new();
    assert(state->ctx);
    vector_init(&state->sockets);
}

int erl_czmq_loop(erl_czmq_state *state) {
    int HANDLER_COUNT = 10;
    cmd_handler handlers[HANDLER_COUNT];
    handlers[0] = &handle_ping;
    handlers[1] = &handle_zsocket_new;
    handlers[2] = &handle_zsocket_type_str;
    handlers[3] = &handle_zsocket_bind;
    handlers[4] = &handle_zsocket_connect;
    handlers[5] = &handle_zsocket_sendmem;
    handlers[6] = &handle_zsocket_destroy;
    handlers[7] = &handle_zstr_send;
    handlers[8] = &handle_zstr_recv_nowait;
    handlers[9] = &handle_zframe_recv_nowait;

    int cmd_len;
    byte cmd_buf[CMD_BUF_SIZE];

    while (1) {
        cmd_len = read_cmd(CMD_BUF_SIZE, cmd_buf);
        if (cmd_len == 0) {
            exit(EXIT_OK);
        } else if (cmd_len < 0) {
            exit(EXIT_PORT_READ_ERROR);
        } else {
            handle_cmd(cmd_buf, state, HANDLER_COUNT, handlers);
        }
    }

    return 0;
}
