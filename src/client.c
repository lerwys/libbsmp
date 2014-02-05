#include "sllp_priv.h"
#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

struct sllp_client
{
    bool                        initialized;
    sllp_comm_func_t            send, recv;
    struct sllp_version         server_version;
    struct sllp_var_info_list   vars;
    struct sllp_group_list      groups;
    struct sllp_curve_info_list curves;
    struct sllp_func_info_list  funcs;
};

static char bin_op_code[BIN_OP_COUNT] =
{
    [BIN_OP_AND]    = 'A',
    [BIN_OP_OR]     = 'O',
    [BIN_OP_XOR]    = 'X',
    [BIN_OP_SET]    = 'S',
    [BIN_OP_CLEAR]  = 'C',
    [BIN_OP_TOGGLE] = 'T',
};

struct sllp_message
{
    uint8_t     code;
    uint16_t    payload_size;
    uint8_t     payload[SLLP_MAX_PAYLOAD];
};

#define LIST_CONTAINS(name, list_type, item_type)\
    static bool name##_list_contains(list_type *list, item_type *item){\
        unsigned int i;\
        for(i = 0; i < list->count; ++i)\
            if(list->list+i == item)\
                return true;\
        return false;\
    }

LIST_CONTAINS(vars,     struct sllp_var_info_list,      struct sllp_var_info)
LIST_CONTAINS(groups,   struct sllp_group_list,         struct sllp_group)
LIST_CONTAINS(curves,   struct sllp_curve_info_list,    struct sllp_curve_info)
LIST_CONTAINS(funcs,    struct sllp_func_info_list,     struct sllp_func_info)

static enum sllp_err command(sllp_client_t *client, struct sllp_message *request,
                             struct sllp_message *response)
{
    if(!client || !request || !response)
        return SLLP_ERR_PARAM_INVALID;

    struct
    {
        uint8_t data[SLLP_MAX_MESSAGE];
        uint32_t size;
    }send_buf, recv_buf;

    // Prepare buffer with the message to be sent
    send_buf.data[0] = request->code;      // Code in the first byte
    send_buf.data[1] = request->payload_size >> 8;
    send_buf.data[2] = request->payload_size;

    // Payload in the subsequent bytes
    memcpy(&send_buf.data[SLLP_HEADER_SIZE], request->payload,
           request->payload_size);

    // Send request
    send_buf.size = SLLP_HEADER_SIZE + request->payload_size;
    if(client->send(send_buf.data, &send_buf.size))
        return SLLP_ERR_COMM;

    // Receive response
    if(client->recv(recv_buf.data, &recv_buf.size))
        return SLLP_ERR_COMM;

    // Must receive, at least, command and size
    if(recv_buf.size < 2)
        return SLLP_ERR_COMM;

    // Copy command code and get payload size
    response->code = recv_buf.data[0];
    response->payload_size = (recv_buf.data[1] << 8) | recv_buf.data[2];

    // Copy response
    memcpy(response->payload, &recv_buf.data[SLLP_HEADER_SIZE], recv_buf.size);

    return SLLP_SUCCESS;
}

static enum sllp_err get_version(sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request =
    {
        .code = CMD_QUERY_VERSION,
        .payload_size = 0
    };

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    // Special case: v1.0
    if(response.code == CMD_ERR_OP_NOT_SUPPORTED)
    {
        client->server_version.major    = 1;
        client->server_version.minor    = 0;
        client->server_version.revision = 0;
    }
    else
    {
        client->server_version.major    = response.payload[0];
        client->server_version.minor    = response.payload[1];
        client->server_version.revision = response.payload[2];
    }

    struct sllp_version *v = &client->server_version;
    snprintf(v->str, SLLP_VERSION_STR_MAX_LEN, "%d.%02d.%03d", v->major,
             v->minor, v->revision);

    return SLLP_SUCCESS;
}

static enum sllp_err update_vars_list(sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request =
    {
        .code = CMD_VAR_QUERY_LIST,
        .payload_size = 0
    };

    if(command(client, &request, &response) || response.code != CMD_VAR_LIST)
        return SLLP_ERR_COMM;

    // Zero list
    memset(&client->vars, 0, sizeof(client->vars));

    // Number of bytes in the payload corresponds to the number of vars in the
    // server
    client->vars.count = response.payload_size;

    unsigned int i;
    for(i = 0; i < client->vars.count; ++i)
    {
        client->vars.list[i].id       = i;
        client->vars.list[i].writable = response.payload[i] & WRITABLE_MASK;
        client->vars.list[i].size     = response.payload[i] & SIZE_MASK;

        if(!client->vars.list[i].size)
            client->vars.list[i].size = SLLP_VAR_MAX_SIZE;
    }

    return SLLP_SUCCESS;
}

static enum sllp_err update_groups_list(sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request =
    {
        .code = CMD_GROUP_QUERY_LIST,
        .payload_size = 0
    };

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    if(response.code != CMD_GROUP_LIST)
        return SLLP_ERR_COMM;           // TODO: better error code

    // Zero list
    memset(&client->groups, 0, sizeof(client->groups));

    // Number of bytes in the payload corresponds to the number of groups in the
    // server
    client->groups.count = response.payload_size;

    // Fill information for each group
    enum sllp_err err_code;
    unsigned int i;
    for(i = 0; i < client->groups.count; ++i)
    {
        // Fill in its info
        struct sllp_group *grp = &client->groups.list[i];

        grp->id         = i;
        grp->size       = 0;
        grp->writable   = response.payload[i] & WRITABLE_MASK;
        grp->vars.count = response.payload[i] & SIZE_MASK;

        // Query each group's variables list
        struct sllp_message grp_response, grp_request = {
            .code           = CMD_GROUP_QUERY,
            .payload_size   = 1,
            .payload        = {i}
        };

        if(command(client, &grp_request, &grp_response) ||
                   grp_response.code != CMD_GROUP)
        {
            err_code = SLLP_ERR_COMM;
            goto err;
        }

        // Each byte in the response is a variable id
        unsigned int j;
        struct sllp_var_info *var;
        for(j = 0; j < grp_response.payload_size; ++j)
        {
            var = &client->vars.list[grp_response.payload[j]];
            grp->vars.list[j] = var;
            grp->size += var->size;
        }
    }

    return SLLP_SUCCESS;

err:
    client->groups.count = 0;
    return err_code;
}

static enum sllp_err update_curves_list(sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request =
    {
        .code = CMD_CURVE_QUERY_LIST,
        .payload_size = 0
    };

    if(command(client, &request, &response) || response.code != CMD_CURVE_LIST)
        return SLLP_ERR_COMM;

    // Zero list
    memset(&client->curves, 0, sizeof(client->curves));

    // Each 3-byte block in the response correspond to a curve
    client->curves.count = response.payload_size/SLLP_CURVE_LIST_INFO;

    unsigned int i;
    uint8_t *payloadp = response.payload;
    for(i = 0; i < client->curves.count; ++i)
    {
        struct sllp_curve_info *curve = &client->curves.list[i];

        curve->id            = i;
        curve->writable      = *(payloadp++);
        curve->block_size    = *(payloadp++) << 8;
        curve->block_size   += *(payloadp++);
        curve->nblocks       = *(payloadp++) << 8;
        curve->nblocks      += *(payloadp++);

        if(!curve->nblocks)
            curve->nblocks = SLLP_CURVE_MAX_BLOCKS;

        struct sllp_message response_csum, request_csum =
        {
            .code = CMD_CURVE_QUERY_CSUM,
            .payload = {i},
            .payload_size = 1
        };

        if(command(client, &request_csum, &response_csum) ||
           response_csum.code != CMD_CURVE_CSUM)
            continue;

        memcpy(curve->checksum, response_csum.payload, SLLP_CURVE_CSUM_SIZE);
    }

    return SLLP_SUCCESS;
}

static enum sllp_err update_funcs_list(sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request =
    {
        .code = CMD_FUNC_QUERY_LIST,
        .payload_size = 0
    };

    if(command(client, &request, &response) || response.code != CMD_FUNC_LIST)
        return SLLP_ERR_COMM;

    // Zero list
    memset(&client->funcs, 0, sizeof(client->funcs));

    // Number of bytes in the payload corresponds to the number of funcs in the
    // server
    client->funcs.count = response.payload_size;

    unsigned int i;
    for(i = 0; i < client->funcs.count; ++i)
    {
        client->funcs.list[i].id            = i;
        client->funcs.list[i].input_size    = (response.payload[i] & 0xF0) >> 4;
        client->funcs.list[i].output_size   = (response.payload[i] & 0x0F);
    }

    return SLLP_SUCCESS;
}

sllp_client_t *sllp_client_new (sllp_comm_func_t send_func,
                                sllp_comm_func_t recv_func)
{
    if(!send_func || !recv_func)
        return NULL;

    struct sllp_client *client = malloc(sizeof(*client));

    if(!client)
        return NULL;

    client->send = send_func;
    client->recv = recv_func;

    client->vars.count = 0;
    memset(&client->vars, 0, sizeof(client->vars));

    client->groups.count = 0;
    memset(&client->groups, 0, sizeof(client->groups));

    client->curves.count = 0;
    memset(&client->curves, 0, sizeof(client->curves));

    client->funcs.count = 0;
    memset(&client->funcs, 0, sizeof(client->funcs));

    client->initialized = false;

    return client;
}

enum sllp_err sllp_client_destroy (sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    free(client);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_client_init(sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    enum sllp_err err;

    if((err = get_version(client)))
        return err;

    if((err = update_vars_list(client)))
        return err;

    if((err = update_groups_list(client)))
        return err;

    if((err = update_curves_list(client)))
        return err;

    if((err = update_funcs_list(client)))
        return err;

    client->initialized = true;
    return SLLP_SUCCESS;
}

#define SLLP_GET_LIST(name, type)\
    enum sllp_err sllp_get_##name##_list (sllp_client_t *client, type **list) {\
        if(!client || !list)\
            return SLLP_ERR_PARAM_INVALID;\
        *list = &client->name;\
        return SLLP_SUCCESS;\
    }

SLLP_GET_LIST(vars,     struct sllp_var_info_list)
SLLP_GET_LIST(groups,   struct sllp_group_list)
SLLP_GET_LIST(curves,   struct sllp_curve_info_list)
SLLP_GET_LIST(funcs,    struct sllp_func_info_list)

struct sllp_version *sllp_get_version(sllp_client_t *client)
{
    if(client)
        return &client->server_version;
    return NULL;
}

enum sllp_err sllp_read_var (sllp_client_t *client, struct sllp_var_info *var,
                             uint8_t *value)
{
    if(!client || !var || !value)
        return SLLP_ERR_PARAM_INVALID;

    if(!vars_list_contains(&client->vars, var))
        return SLLP_ERR_PARAM_INVALID;

    // Prepare message to be sent
    struct sllp_message response, request =
    {
        .code = CMD_VAR_READ,
        .payload = {var->id},
        .payload_size = 1

    };

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    if(response.code != CMD_VAR_VALUE)
        return SLLP_ERR_COMM;   //TODO: better error?

    // Give back answer
    memcpy(value, response.payload, response.payload_size);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_write_var (sllp_client_t *client, struct sllp_var_info *var,
                              uint8_t *value)
{
    if(!client || !var || !value)
        return SLLP_ERR_PARAM_INVALID;

    if(!vars_list_contains(&client->vars, var))
        return SLLP_ERR_PARAM_INVALID;

    if(!var->writable)
        return SLLP_ERR_PARAM_INVALID;

    // Prepare message to be sent
    struct sllp_message request = {
        .code = CMD_VAR_WRITE,
        .payload = {var->id},
        .payload_size = 1 + var->size
    }, response;

    memcpy(&request.payload[1], value, var->size);

    if(command(client, &request, &response))
       return SLLP_ERR_COMM;

    if(response.code != CMD_OK)
       return SLLP_ERR_COMM;   //TODO: better error?

    return SLLP_SUCCESS;
}

enum sllp_err sllp_write_read_vars (sllp_client_t *client,
                                    struct sllp_var_info *write_var,
                                    uint8_t *write_value,
                                    struct sllp_var_info *read_var,
                                    uint8_t *read_value)
{
    if(!(client && write_var && write_value && read_var && read_value))
        return SLLP_ERR_PARAM_INVALID;

    if(!vars_list_contains(&client->vars, write_var) || !write_var->writable)
        return SLLP_ERR_PARAM_INVALID;

    if(!vars_list_contains(&client->vars, read_var))
        return SLLP_ERR_PARAM_INVALID;

    // Prepare message to be sent
    struct sllp_message request = {
        .code = CMD_VAR_WRITE_READ,
        .payload = {write_var->id, read_var->id},
        .payload_size = 2 + write_var->size
    }, response;

    memcpy(&request.payload[2], write_value, write_var->size);

    if(command(client, &request, &response))
       return SLLP_ERR_COMM;

    if(response.code != CMD_VAR_VALUE)
       return SLLP_ERR_COMM;   //TODO: better error?

    memcpy(read_value, response.payload, read_var->size);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_read_group (sllp_client_t *client, struct sllp_group *grp,
                               uint8_t *values)
{
    if(!client || !grp || !values)
        return SLLP_ERR_PARAM_INVALID;

    if(!groups_list_contains(&client->groups, grp))
        return SLLP_ERR_PARAM_INVALID;

    // Prepare message to be sent
    struct sllp_message response, request = {
        .code = CMD_GROUP_READ,
        .payload = {grp->id},
        .payload_size = 1
    };

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    if(response.code != CMD_GROUP_VALUES)
        return SLLP_ERR_COMM;   //TODO: better error?

    // Give back answer
    memcpy(values, response.payload, response.payload_size);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_write_group (sllp_client_t *client, struct sllp_group *grp,
                                uint8_t *values)
{
    if(!client || !grp || !values)
        return SLLP_ERR_PARAM_INVALID;

    if(!groups_list_contains(&client->groups, grp))
        return SLLP_ERR_PARAM_INVALID;

    if(!grp->writable)
        return SLLP_ERR_PARAM_INVALID;

    // Prepare message to be sent
    struct sllp_message response, request = {
        .code = CMD_GROUP_WRITE,
        .payload = {grp->id},
        .payload_size = 1 + grp->size
    };

    memcpy(&request.payload[1], values, grp->size);

    if(command(client, &request, &response))
       return SLLP_ERR_COMM;

    if(response.code != CMD_OK)
       return SLLP_ERR_COMM;   //TODO: better error?

    return SLLP_SUCCESS;
}

enum sllp_err sllp_bin_op_var (sllp_client_t *client, enum sllp_bin_op op,
                               struct sllp_var_info *var, uint8_t *mask)
{
    if(!client || !var || !mask)
        return SLLP_ERR_PARAM_INVALID;

    if(!vars_list_contains(&client->vars, var))
        return SLLP_ERR_PARAM_INVALID;

    if(!var->writable)
        return SLLP_ERR_PARAM_INVALID;

    if(op >= BIN_OP_COUNT)
        return SLLP_ERR_PARAM_OUT_OF_RANGE;

    // Prepare message to be sent
    struct sllp_message response, request = {
        .code = CMD_VAR_BIN_OP,
        .payload = {var->id, bin_op_code[op]},
        .payload_size = 2 + var->size
    };

    memcpy(&request.payload[2], mask, var->size);

    if(command(client, &request, &response))
       return SLLP_ERR_COMM;

    if(response.code != CMD_OK)
       return SLLP_ERR_COMM;   //TODO: better error?

    return SLLP_SUCCESS;
}

enum sllp_err sllp_bin_op_group (sllp_client_t *client, enum sllp_bin_op op,
                                 struct sllp_group *grp, uint8_t *mask)
{
    if(!client || !grp || !mask)
        return SLLP_ERR_PARAM_INVALID;

    if(!groups_list_contains(&client->groups, grp))
        return SLLP_ERR_PARAM_INVALID;

    if(!grp->writable)
        return SLLP_ERR_PARAM_INVALID;

    if(op >= BIN_OP_COUNT)
        return SLLP_ERR_PARAM_OUT_OF_RANGE;

    // Prepare message to be sent
    struct sllp_message response, request = {
        .code = CMD_GROUP_BIN_OP,
        .payload = {grp->id, bin_op_code[op]},
        .payload_size = 2 + grp->size
    };

    memcpy(&request.payload[2], mask, grp->size);

    if(command(client, &request, &response))
       return SLLP_ERR_COMM;

    if(response.code != CMD_OK)
       return SLLP_ERR_COMM;   //TODO: better error?

    return SLLP_SUCCESS;
}

enum sllp_err sllp_create_group (sllp_client_t *client,
                                 struct sllp_var_info **list)
{
    if(!client || !list || !(*list))
        return SLLP_ERR_PARAM_INVALID;

    // Prepare message to be sent
    struct sllp_message request = {
        .code = CMD_GROUP_CREATE,
        .payload_size = 0
    }, response;

    while(*list)
    {
        if(!vars_list_contains(&client->vars, *list))
            return SLLP_ERR_PARAM_INVALID;

        request.payload[request.payload_size++] = (*(list++))->id;
    }

    if(!request.payload_size)
        return SLLP_ERR_PARAM_INVALID;

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    if(response.code != CMD_OK)
        return SLLP_ERR_COMM;

    update_groups_list(client);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_remove_all_groups (sllp_client_t *client)
{
    if(!client)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request = {
        .code = CMD_GROUP_REMOVE_ALL,
        .payload_size = 0
    };

    if(command(client, &request, &response) || response.code != CMD_OK)
        return SLLP_ERR_COMM;

    update_groups_list(client);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_request_curve_block (sllp_client_t *client,
                                        struct sllp_curve_info *curve,
                                        uint16_t offset, uint8_t *data,
                                        uint16_t *len)
{
    if(!client || !curve || !data || !len)
        return SLLP_ERR_PARAM_INVALID;

    if(!curves_list_contains(&client->curves, curve))
        return SLLP_ERR_PARAM_INVALID;

    if(offset > curve->nblocks)
        return SLLP_ERR_PARAM_OUT_OF_RANGE;

    struct sllp_message response, request = {
        .code = CMD_CURVE_BLOCK_REQUEST,
        .payload = {curve->id, offset >> 8, offset},
        .payload_size = SLLP_CURVE_BLOCK_INFO
    };

    if(command(client, &request, &response) || response.code !=CMD_CURVE_BLOCK)
        return SLLP_ERR_COMM;

    *len = response.payload_size - SLLP_CURVE_BLOCK_INFO;
    memcpy(data, response.payload + SLLP_CURVE_BLOCK_INFO, *len);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_send_curve_block (sllp_client_t *client,
                                     struct sllp_curve_info *curve,
                                     uint16_t offset, uint8_t *data,
                                     uint16_t len)
{
    if(!client || !curve || !data)
        return SLLP_ERR_PARAM_INVALID;

    if(!curves_list_contains(&client->curves, curve))
        return SLLP_ERR_PARAM_INVALID;

    if(!curve->writable)
        return SLLP_ERR_PARAM_INVALID;

    if(offset > curve->nblocks)
        return SLLP_ERR_PARAM_OUT_OF_RANGE;

    if(len > curve->block_size)
        return SLLP_ERR_PARAM_OUT_OF_RANGE;

    struct sllp_message response, request = {
        .code = CMD_CURVE_BLOCK,
        .payload = {curve->id, offset >> 8, offset},
        .payload_size = len + SLLP_CURVE_BLOCK_INFO,
    };

    memcpy(request.payload + SLLP_CURVE_BLOCK_INFO, data, len);

    if(command(client, &request, &response) || response.code != CMD_OK)
        return SLLP_ERR_COMM;

    return SLLP_SUCCESS;
}

enum sllp_err sllp_recalc_checksum (sllp_client_t *client,
                                    struct sllp_curve_info *curve)
{
    if(!client || !curve)
        return SLLP_ERR_PARAM_INVALID;

    if(!curves_list_contains(&client->curves, curve))
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request = {
        .code = CMD_CURVE_RECALC_CSUM,
        .payload = {curve->id},
        .payload_size = 1
    };

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    if(response.code != CMD_OK)
        return SLLP_ERR_COMM;

    update_curves_list(client);

    return SLLP_SUCCESS;
}

enum sllp_err sllp_func_execute (sllp_client_t *client,
                                 struct sllp_func_info *func, uint8_t *error,
                                 uint8_t *input, uint8_t *output)
{
    if(!client || !func || !error)
        return SLLP_ERR_PARAM_INVALID;

    if(!funcs_list_contains(&client->funcs, func))
        return SLLP_ERR_PARAM_INVALID;

    if(func->input_size && !input)
        return SLLP_ERR_PARAM_INVALID;

    if(func->output_size && !output)
        return SLLP_ERR_PARAM_INVALID;

    struct sllp_message response, request = {
        .code = CMD_FUNC_EXECUTE,
        .payload = {func->id},
        .payload_size = 1 + func->input_size
    };

    if(func->input_size)
        memcpy(&request.payload[1], input, func->input_size);

    if(command(client, &request, &response))
        return SLLP_ERR_COMM;

    if(response.code == CMD_FUNC_RETURN)
    {
        *error = 0;
        if(func->output_size)
            memcpy(output, response.payload, func->output_size);
        return SLLP_SUCCESS;
    }
    else if(response.code == CMD_FUNC_ERROR)
    {
        *error = response.payload[0];
        return SLLP_SUCCESS;
    }
    else
        return SLLP_ERR_COMM;
}
