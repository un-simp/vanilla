#include <stdio.h>
#include <stdlib.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>

static AvahiEntryGroup *group = NULL;
static AvahiSimplePoll *simple_poll = NULL;
static char *service_name = NULL;

static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    if (state == AVAHI_ENTRY_GROUP_ESTABLISHED) {
        fprintf(stderr, "SERVICE '%s' SUCCESSFULLY ESTABLISHED.\n", service_name);
    } else if (state == AVAHI_ENTRY_GROUP_FAILURE) {
        fprintf(stderr, "ENTRY GROUP FAILURE: %s\n", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
        avahi_simple_poll_quit(simple_poll);
    }
}

static void create_service(AvahiClient *client) {
    // Create a new entry group if necessary
    if (!group) {
        group = avahi_entry_group_new(client, entry_group_callback, NULL);
        if (!group) {
            fprintf(stderr, "FAILED TO CREATE ENTRY GROUP: %s\n", avahi_strerror(avahi_client_errno(client)));
            return;
        }
    }

    // If the group is empty, add our service
    if (avahi_entry_group_is_empty(group)) {
        fprintf(stderr, "ADDING SERVICE '%s'\n", service_name);
        int ret = avahi_entry_group_add_service(
            group,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC,
            0,
            service_name,
            "_vanillau._udp",
            NULL,
            NULL,
            8096,
            NULL
        );

        if (ret < 0) {
            fprintf(stderr, "FAILED TO ADD SERVICE: %s\n", avahi_strerror(ret));
            return;
        }

        // Commit the entry group to register the service
        ret = avahi_entry_group_commit(group);
        if (ret < 0) {
            fprintf(stderr, "FAILED TO COMMIT ENTRY GROUP: %s\n", avahi_strerror(ret));
        }
    }
}

static void client_callback(AvahiClient *client, AvahiClientState state, void *userdata) {
    if (state == AVAHI_CLIENT_S_RUNNING) {
        create_service(client);
    } else if (state == AVAHI_CLIENT_FAILURE) {
        fprintf(stderr, "CLIENT FAILURE: %s\n", avahi_strerror(avahi_client_errno(client)));
        avahi_simple_poll_quit(simple_poll);
    }
}

void* regService() {
    AvahiClient *client = NULL;
    int error;

    // Allocate a simple poll object
    simple_poll = avahi_simple_poll_new();
    if (!simple_poll) {
        fprintf(stderr, "FAILED TO CREATE POLL OBJECT.\n");
        return (void*)1;
    }

    // Set the service name
    service_name = "Vanilla U";

    // Create a new Avahi client
    client = avahi_client_new(avahi_simple_poll_get(simple_poll), 0, client_callback, NULL, &error);
    if (!client) {
        fprintf(stderr, "FAILED TO CREATE CLIENT: %s\n", avahi_strerror(error));
        avahi_simple_poll_free(simple_poll);
        return (void*)1;
    }

    // Run the main loop
    avahi_simple_poll_loop(simple_poll);

    // Cleanup
    avahi_client_free(client);
    avahi_simple_poll_free(simple_poll);
    return (void*)0;
}
