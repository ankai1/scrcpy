#include "receiver.h"

#include <SDL2/SDL_assert.h>
#include <SDL2/SDL_events.h>

#include "config.h"
#include "device_event.h"
#include "events.h"
#include "lock_util.h"
#include "log.h"

bool
receiver_init(struct receiver *receiver, socket_t control_socket) {
    if (!(receiver->mutex = SDL_CreateMutex())) {
        return false;
    }
    receiver->control_socket = control_socket;
    receiver->clipboard_text = NULL;
    return true;
}

void
receiver_destroy(struct receiver *receiver) {
    SDL_DestroyMutex(receiver->mutex);
    SDL_free(receiver->clipboard_text);
}

static void
process_event(struct receiver *receiver, struct device_event *event) {
    switch (event->type) {
        case DEVICE_EVENT_TYPE_CLIPBOARD:
            mutex_lock(receiver->mutex);
            // harmless if SDL_strdup() returns NULL
            receiver->clipboard_text = SDL_strdup(event->clipboard_event.text);
            mutex_unlock(receiver->mutex);

            static SDL_Event device_clipboard_event = {
                .type = EVENT_DEVICE_CLIPBOARD,
            };
            SDL_PushEvent(&device_clipboard_event);
            break;
    }
}

static ssize_t
process_events(struct receiver *receiver, const unsigned char *buf,
               size_t len) {
    size_t head = 0;
    for (;;) {
        struct device_event event;
        ssize_t r = device_event_deserialize(&buf[head], len - head,
                                             &event);
        if (r == -1) {
            return -1;
        }
        if (r == 0) {
            return head;
        }

        process_event(receiver, &event);

        head += r;
        SDL_assert(head <= len);
        if (head == len) {
            return head;
        }
    }
}

static int
run_receiver(void *data) {
    struct receiver *receiver = data;

    unsigned char buf[DEVICE_EVENT_SERIALIZED_MAX_SIZE];
    size_t head = 0;

    for (;;) {
        ssize_t r = net_recv(receiver->control_socket, buf,
                             DEVICE_EVENT_SERIALIZED_MAX_SIZE - head);
        SDL_assert(r);
        if (r == -1) {
            LOGD("Receiver stopped");
            break;
        }

        ssize_t consumed = process_events(receiver, buf, r);
        if (consumed == -1) {
            // an error occurred
            break;
        }

        if (consumed) {
            // shift the remaining data in the buffer
            memmove(buf, &buf[consumed], r - consumed);
            head = r - consumed;
        }
    }

    return 0;
}

bool
receiver_start(struct receiver *receiver) {
    LOGD("Starting receiver thread");

    receiver->thread = SDL_CreateThread(run_receiver, "receiver", receiver);
    if (!receiver->thread) {
        LOGC("Could not start receiver thread");
        return false;
    }

    return true;
}

void
receiver_join(struct receiver *receiver) {
    SDL_WaitThread(receiver->thread, NULL);
}

char *
receiver_consume_device_clipboard(struct receiver *receiver) {
    mutex_lock(receiver->mutex);

    char *clipboard_text = receiver->clipboard_text;
    receiver->clipboard_text = NULL;

    mutex_unlock(receiver->mutex);

    return clipboard_text;
}
