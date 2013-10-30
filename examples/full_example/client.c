/*
 * Sirius Low Level Protocol Library - libsllp
 * Client example
 *
 *
 * This is a fully documented libsllp client example. This is a good place to
 * see the API in action.
 */

/* Some boilerplate */
#include "server.h" // Here 'server' will replace our communications functions
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C "CLIENT: "

#define TRY(name, func)\
    do {\
        enum sllp_err err = func;\
        if(err) {\
            fprintf(stderr, C name": %s\n", sllp_error_str(err));\
            exit(-1);\
        }\
    }while(0)

#undef PRINT_PACKET
#ifdef PRINT_PACKET
static void print_packet(char* pre, uint8_t *data, uint32_t len)
{
    unsigned int i;

    printf("%s", pre);
    if(len > 30)
        printf("[%2X %2X ] + %d bytes of payload\n", data[0], data[1], len-2);
    else
    {
        printf("[");
        for(i = 0; i < len; ++i)
            printf("%2X ", data[i]);
        printf("]\n");
    }
}
#endif


/*
 * The very first thing to do is to include the client library definitions. This
 * is done with this directive. Nothing else SLLP-related is needed.
 */
#include <sllp_client.h>

/*
 * We need to define two communication functions to be used by the client.
 * As we are just emulating communications our functions will be very simple.
 * On a real client you might want to do real communication (via sockets, for
 * instance). Here our communication is a simple function call.
 */

/*
 * These buffers will hold the messages to be exchanged.
 */
static struct
{
    uint8_t data[SLLP_MAX_MESSAGE];
    uint32_t len;
}recv_buffer, send_buffer;

/*
 * The server A/D's are, of course, 16-bit. They are bipolar, from -10V to +10V.
 * We use this function to convert the bytes that we've got.
 */
static double convert_ad(uint8_t value[2])
{
    return (((value[0] << 8) | value[1])/65535.0)*20.0 - 10.0;
}

/*
 * The send function copy the data to be sent into the appropriate buffer, ask
 * the server to interpret the message and store the result in the other buffer.
 */

static int client_send (uint8_t *data, uint32_t *count)
{
    memcpy(send_buffer.data, data, *count);
    send_buffer.len = *count;

#ifdef PRINT_PACKET
    print_packet(" REQUEST: ", send_buffer.data, send_buffer.len);
#endif

    server_process_message(send_buffer.data, send_buffer.len,
                           recv_buffer.data, &recv_buffer.len);

#ifdef PRINT_PACKET
    print_packet("RESPONSE: ", recv_buffer.data, recv_buffer.len);
#endif

    return 0;
}

/*
 * The recv function just gets whatever is in the recv_buffer and copy into the
 * appropriate places.
 */
static int client_recv (uint8_t *data, uint32_t *count)
{
    memcpy(data, recv_buffer.data, recv_buffer.len);
    *count = recv_buffer.len;

    return 0;
}

int main(void)
{
    unsigned int i; // Just a counter. Never mind it.

    puts("-------------------------------------------------------------------");
    puts("This is an example of usage of the libsllp. This output makes      ");
    puts("more sense if you open the source code '"__FILE__"' and read along.");
    puts("-------------------------------------------------------------------");
    puts("");

    /*
     * We need to initialize our toy server. This wouldn't be needed in a real
     * client, because the server would reside elsewhere (in another thread, or
     * in another computer).
     */
    server_init();

    /*
     * Okay, let's begin our journey of creating and using a client of the SLLP.
     *
     * Firstly you shall create an instance for the client. The instance is
     * malloc'ed and returned. You have to pass your communications functions.
     */
    sllp_client_t *client = sllp_client_new(client_send, client_recv);

    if(!client)
    {
        fprintf(stderr, C"Couldn't allocate client instance.\n");
        return -1;
    }

    /*
     * Initialize our client. Initialization does a lot of communications with
     * the server, so bear in mind that your communications should be ready to
     * be used before the call to this function.
     */
    TRY("init", sllp_client_init(client));

    /*
     * If we got past the last line, we now have a new shiny client, waiting to
     * be used!
     */

    /*
     * We can, for starters, get a list of all the Variables in the server:
     */
    struct sllp_var_info_list *vars;
    TRY("vars_list", sllp_get_vars_list(client, &vars));

    printf(C"Server has %d Variable(s):\n", vars->count);
    for(i = 0; i < vars->count; ++i)
        printf(C" ID[%d] SIZE[%2d] WRITABLE[%s]\n",
                vars->list[i].id,
                vars->list[i].size,
                vars->list[i].writable ? "true " : "false");

    /*
     * How about a list of groups?
     */
    struct sllp_group_list *groups;
    TRY("groups_list", sllp_get_groups_list(client, &groups));

    printf("\n"C"Server has %d Group(s):\n", groups->count);
    for(i = 0; i < groups->count; ++i)
    {
        printf(C" ID[%d] SIZE[%2d] WRITABLE[%s] VARS[",
                groups->list[i].id,
                groups->list[i].size,
                groups->list[i].writable ? "true " : "false");

        unsigned int j;
        for(j = 0; j < groups->list[i].vars.count; ++j)
            printf("%2d ", groups->list[i].vars.list[j]->id);
        printf("]\n");
    }

    /*
     * Hmm cool! Easy! Now, Curves!
     */
    struct sllp_curve_info_list *curves;
    TRY("curves_list", sllp_get_curves_list(client, &curves));

    printf("\n"C"Server has %d Curve(s):\n", curves->count);
    for(i = 0; i < curves->count; ++i)
        printf(C" ID[%d] BLOCKS[%3d (%7d bytes)] WRITABLE[%s]\n",
                curves->list[i].id,
                curves->list[i].nblocks,
                (curves->list[i].nblocks)*SLLP_CURVE_BLOCK_SIZE,
                curves->list[i].writable ? "true " : "false");

    /*
     * Alright alright, last but no least, let's ask the server what are his
     * Functions.
     */
    struct sllp_func_info_list *funcs;
    TRY("funcs_list", sllp_get_funcs_list(client, &funcs));

    printf("\n"C"Server has %d Functions(s):\n", funcs->count);
    for(i = 0; i < funcs->count; ++i)
        printf(C" ID[%d] INPUT[%2d bytes] OUTPUT[%2d bytes]\n",
                funcs->list[i].id,
                funcs->list[i].input_size,
                funcs->list[i].output_size);

    /*
     * At this point we know all the Entities in the server. We can start
     * manipulating them!
     */

    /*
     * First, let's read some Variables. According to the "documentation" of our
     * toy server, the first Variable contains the name of the server. Let's see
     * what is his name.
     */
    printf("\n");
    struct sllp_var_info *var_name = &vars->list[0];
    uint8_t server_name[var_name->size];

    TRY("read_server_name", sllp_read_var(client, var_name, server_name));
    printf(C"Server said his name was %s. Hello %s!\n", (char*) server_name,
            server_name);

    /*
     * This Variable is read-only. What if we try to change the name of the
     * server?
     */
    uint8_t new_server_name[var_name->size];
    strcpy(new_server_name, "Tiny little server");
    printf(C"Let's try to change the server name to '%s'...\n",new_server_name);

    if(!sllp_write_var(client, var_name, new_server_name))
        printf(C"  Yes! We changed the server name! This library is lame.\n");
    else
        printf(C"  Crap. The server refuses to change his name... If it "
                "wasn't for this meddling library!\n");
    /*
     * As you could see, it was impossible to change the server name. If you pay
     * more attention, you will notice that the message "SERVER: Request to
     * WRITE to the Variables..." wasn't printed. That's because the server
     * never knew we tried to write on a read-only Variable. The attempt was
     * blocked by the client library. Even if you managed to send a message like
     * this to the server, the server side library would return an error as
     * well, not writing anything to the Variable.
     */

    /*
     * Moving on, we will read the first A/D converter. According to our well-
     * written server manual, this is the second Variable.
     */
    printf("\n");
    struct sllp_var_info *var_1st_ad = &vars->list[1];
    uint8_t ad_value[var_1st_ad->size];
    TRY("read 1st ad", sllp_read_var(client, var_1st_ad, ad_value));

    /*
     * It's, of course, a bipolar A/D converter, from -10V to +10V. We need to
     * convert the bytes that we've got.
     */
    printf(C"The 1st A/D converter is 'reading' %.3f V. Weird...\n",
           convert_ad(ad_value));

    /*
     * The A/D is showing -10.0V! Oh, of course! We didn't "start" the
     * "conversion"! What a silly fake A/D. Let's do that. We shall call a
     * function. Our server manual (which is the source code server.c) says that
     * the function to start the A/D conversions is the first one.
     */
    struct sllp_func_info *func_convert_ads = &funcs->list[0];
    printf(C"Server, start the conversions of the A/D converters. NOW!!!\n");
    uint8_t convert_ads_error;
    TRY("convert ads", sllp_func_execute(client, func_convert_ads,
                                         &convert_ads_error, NULL, NULL));

    TRY("reread 1st ad", sllp_read_var(client, var_1st_ad, ad_value));
    printf(C"The 1st A/D converter is now 'reading' %.3f V! Nice!\n",
            convert_ad(ad_value));

    /*
     * Remember that the server has 2 A/D's? What if we wanted to read both of
     * them with just one command? Is that possible? Yes we can! I mean, yes it
     * is. We just create a new Group first. We have to pass a NULL-terminated
     * list of Variables. The A/D's are the second and the third variables.
     */
    printf("\n"C"Creating a group with both A/D converters in it\n");
    struct sllp_var_info *all_ads[3] = {&vars->list[1], &vars->list[2], NULL};
    TRY("create group", sllp_create_group(client, all_ads));

    /* The group created is the last one */
    struct sllp_group *ads_group = &groups->list[groups->count-1];
    uint8_t ads_values[ads_group->size];

    printf(C"Now the server has %d groups. The last group contains %d "
            "Variables.\n", groups->count, ads_group->vars.count);

    printf(C"Let's read this group. It contains our A/D's.\n");

    TRY("read group", sllp_read_group(client, ads_group, ads_values));
    printf(C"  1st A/D = %.3f V    2nd A/D = %.3f V\n",
            convert_ad(&ads_values[0]), convert_ad(&ads_values[1]));

    /*
     * Great! We read from two Variables with only one command! What a powerful
     * library! Now, we don't want those A/D's in a group anymore. In fact, we
     * don't want any groups anymore! What? You do? Well, I don't. I'll get rid
     * of all of them! MUAHAHAHAHA!
     */
    printf("\n"C"Ok, enough of groups. I'll remove them all!\n");
    TRY("remove groups", sllp_remove_all_groups(client));
    printf(C"Done. Now the sever has... What? %d groups??\n", groups->count);
    printf(C"Oh yeah, of course, there are 3 irremovable standard groups...\n");

    /*
     * It's not possible to remove the first three groups... Bummer! Well, at
     * least you have some of your *precious* groups.
     */

    /*
     * We covered a lot of commands so far. Let's check these nifty binary
     * operations. Suppose our server has a missile launcher AND an atomic bomb.
     * Now suppose you are an evil warlord. You want to shoot that missile, but
     * you must not detonate the bomb, otherwise you'll die.
     *
     * The missile launches when the most significant bit of the server's
     * digital output toggles. Likewise, the bomb explodes when the least
     * significant bit of the digital toggles. You have to do that in only one
     * command. Why? Because I make the rules!
     *
     * The library comes to the rescue! You can toggle any bit of any Variable
     * without knowing its previous value.
     *
     * The digital output Variable is the fourth one.
     */

    struct sllp_var_info *var_dig_output = &vars->list[3];
    uint8_t toggle_mask[var_dig_output->size];
    toggle_mask[0] = 0x80; // Most significant bit

    printf("\n"C"Let's try to toggle the most significant bit of the digital "
           "output\n");
    TRY("toggle bit", sllp_bin_op_var(client, BIN_OP_TOGGLE, var_dig_output,
                                      toggle_mask));
    /*
     * Missile launched!!
     */
    printf(C"Done!\n");



    return 0;
}
