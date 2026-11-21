/*
 * test_server_context.c - the command context of the server, launch and attach.
 *
 * Runs dap_server_handle_command() on a server that has no client (the transport is
 * created but never started), as a debug adapter's own thread does after it has read
 * a request:
 *   - launch after other commands: the context union of the server holds what the
 *     previous command left in it; launch fills no member of its own, and the cleanup
 *     after it used to free those bytes as the pointers of the launch member
 *     (nd100x crashed, signal 11, on a launch sent to a running machine);
 *   - attach with mapFile, sourceFile, sourcePaths reads them into debugger_state and
 *     calls the DAP_CMD_ATTACH callback, as launch does for its own;
 *   - programLevel, programContext and programFromFirstStop on launch and attach.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "dap_server.h"

static int s_failed = 0;
static int s_checks = 0;
static int s_attach_calls = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        s_checks++;                                                            \
        if (!(cond))                                                           \
        {                                                                      \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);         \
            s_failed++;                                                        \
        }                                                                      \
    } while (0)

static int on_attach(DAPServer *server)
{
    (void)server;
    s_attach_calls++;
    return 0;
}

static int on_launch(DAPServer *server)
{
    (void)server;
    return 0;
}

static int run(DAPServer *server, DAPCommandType cmd, const char *json)
{
    DAPResponse response;
    memset(&response, 0, sizeof(response));
    cJSON *args = cJSON_Parse(json);
    int rc = dap_server_handle_command(server, cmd, NULL, args, &response);
    free(response.error_message);
    free(response.data);
    return (rc < 0 || !response.success) ? -1 : 0;
}

int main(void)
{
    DAPServerConfig config;
    memset(&config, 0, sizeof(config));
    config.transport.type = DAP_TRANSPORT_TCP;
    config.transport.config.tcp.host = "127.0.0.1";
    config.transport.config.tcp.port = 0;
    DAPServer *server = dap_server_create(&config);
    CHECK(server != NULL, "server created");
    if (!server)
    {
        return 1;
    }
    dap_server_register_command_callback(server, DAP_CMD_ATTACH, on_attach);
    dap_server_register_command_callback(server, DAP_CMD_LAUNCH, on_launch);

    /* What an earlier command can leave in the shared context union: bytes that are not
     * pointers of the launch member. */
    memset(&server->current_command.context, 0x5A, sizeof(server->current_command.context));
    CHECK(run(server, DAP_CMD_LAUNCH,
              "{\"program\":\"N100A:PROG\",\"mapFile\":\"N100A.map\",\"programLevel\":1,"
              "\"programContext\":2180}") == 0,
          "launch after another command succeeds (no free of stale context)");
    CHECK(server->debugger_state.map_path && !strcmp(server->debugger_state.map_path, "N100A.map"),
          "launch reads mapFile");
    CHECK(server->debugger_state.program_level == 1 &&
              server->debugger_state.program_context == 2180 &&
              !server->debugger_state.program_from_first_stop,
          "launch reads programLevel and programContext");

    CHECK(run(server, DAP_CMD_ATTACH,
              "{\"mapFile\":\"OTHER.map\",\"sourceFile\":\"N100A.PLNC\","
              "\"sourcePaths\":[\"src\",\"lib\"],\"programFromFirstStop\":true}") == 0,
          "attach with a map file succeeds");
    CHECK(s_attach_calls == 1, "attach calls the DAP_CMD_ATTACH callback");
    CHECK(server->debugger_state.map_path && !strcmp(server->debugger_state.map_path, "OTHER.map"),
          "attach reads mapFile");
    CHECK(server->debugger_state.source_path &&
              !strcmp(server->debugger_state.source_path, "N100A.PLNC"),
          "attach reads sourceFile");
    CHECK(server->debugger_state.source_paths_count == 2 &&
              !strcmp(server->debugger_state.source_paths[1], "lib"),
          "attach reads sourcePaths");
    CHECK(server->debugger_state.program_level == -1 &&
              server->debugger_state.program_context == -1 &&
              server->debugger_state.program_from_first_stop,
          "attach reads programFromFirstStop; level and context not named");

    CHECK(run(server, DAP_CMD_ATTACH, "{}") == 0, "attach with no arguments");
    CHECK(server->debugger_state.map_path == NULL, "attach with no map file clears the old one");

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
