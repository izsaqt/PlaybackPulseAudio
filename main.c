#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdbool.h>

#define MAX_PATH_LENGTH 1024

typedef struct {
    char name[256];
    bool exists;
} SinkInfo;

typedef struct {
    pa_mainloop *mainloop;
    pa_mainloop_api *mainloop_api;
    pa_context *context;
    int ready;
} PAConnection;

void sink_info_callback(pa_context *c, const pa_sink_info *info, int eol, void *userdata) {
    SinkInfo *sink_info = (SinkInfo *)userdata;
    
    if (eol > 0 || !info) {
        return;
    }
    
    if (strcmp(info->name, sink_info->name) == 0) {
        sink_info->exists = true;
    }
}

void context_state_callback(pa_context *c, void *userdata) {
    PAConnection *pa = (PAConnection *)userdata;
    
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa->ready = 1;
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            pa->ready = 2;
            break;
        default:
            break;
    }
}

bool initialize_pa(PAConnection *pa) {
    pa->mainloop = pa_mainloop_new();
    if (!pa->mainloop) {
        fprintf(stderr, "Failed to create mainloop\n");
        return false;
    }
    
    pa->mainloop_api = pa_mainloop_get_api(pa->mainloop);
    if (!pa->mainloop_api) {
        fprintf(stderr, "Failed to get mainloop API\n");
        pa_mainloop_free(pa->mainloop);
        return false;
    }
    
    pa->context = pa_context_new(pa->mainloop_api, "Audio Loopback");
    if (!pa->context) {
        fprintf(stderr, "Failed to create context\n");
        pa_mainloop_free(pa->mainloop);
        return false;
    }
    
    pa->ready = 0;
    
    pa_context_set_state_callback(pa->context, context_state_callback, pa);
    
    if (pa_context_connect(pa->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        fprintf(stderr, "Failed to connect to PulseAudio server: %s\n", 
                pa_strerror(pa_context_errno(pa->context)));
        pa_context_unref(pa->context);
        pa_mainloop_free(pa->mainloop);
        return false;
    }
    
    while (pa->ready == 0) {
        pa_mainloop_iterate(pa->mainloop, 1, NULL);
    }
    
    if (pa->ready == 2) {
        fprintf(stderr, "Connection to PulseAudio server failed\n");
        pa_context_unref(pa->context);
        pa_mainloop_free(pa->mainloop);
        return false;
    }
    
    return true;
}

void cleanup_pa(PAConnection *pa) {
    if (pa->context) {
        pa_context_disconnect(pa->context);
        pa_context_unref(pa->context);
    }
    
    if (pa->mainloop) {
        pa_mainloop_free(pa->mainloop);
    }
}

bool check_sink_exists(PAConnection *pa, const char *sink_name) {
    SinkInfo sink_info;
    strncpy(sink_info.name, sink_name, sizeof(sink_info.name) - 1);
    sink_info.exists = false;
    
    pa_operation *op = pa_context_get_sink_info_by_name(pa->context, sink_name, sink_info_callback, &sink_info);
    if (op) {
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_mainloop_iterate(pa->mainloop, 1, NULL);
        }
        pa_operation_unref(op);
    }
    
    return sink_info.exists;
}

uint32_t ensure_virtual_sink(PAConnection *pa, const char *sink_name) {
    uint32_t module_index = PA_INVALID_INDEX;
    
    if (!check_sink_exists(pa, sink_name)) {
        printf("Creating virtual sink: %s\n", sink_name);
        
        char module_args[512];
        snprintf(module_args, sizeof(module_args), 
                "sink_name=%s sink_properties=device.description=\"Virtual_Microphone\"", 
                sink_name);
        
        pa_operation *op = pa_context_load_module(pa->context, "module-null-sink", module_args, NULL, NULL);
        if (op) {
            pa_operation_unref(op);
            sleep(1);
        } else {
            fprintf(stderr, "Failed to create virtual sink\n");
        }
    } else {
        printf("Using existing sink: %s\n", sink_name);
    }
    
    return module_index;
}

typedef struct {
    char *sink_name;
} DefaultSinkInfo;

void get_default_sink_callback(pa_context *c, const pa_server_info *info, void *userdata) {
    DefaultSinkInfo *sink_info = (DefaultSinkInfo *)userdata;
    
    if (info && info->default_sink_name) {
        sink_info->sink_name = strdup(info->default_sink_name);
    }
}

char *get_default_sink(PAConnection *pa) {
    DefaultSinkInfo sink_info = { NULL };
    
    pa_operation *op = pa_context_get_server_info(pa->context, get_default_sink_callback, &sink_info);
    if (op) {
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_mainloop_iterate(pa->mainloop, 1, NULL);
        }
        pa_operation_unref(op);
    }
    
    return sink_info.sink_name;
}

uint32_t create_combined_sink(PAConnection *pa, const char *virtual_sink, const char *default_sink) {
    uint32_t module_index = PA_INVALID_INDEX;
    
    pa_operation *op = pa_context_get_module_info_list(pa->context, NULL, NULL);
    if (op) {
        pa_operation_unref(op);
    }
    
    char module_args[512];
    snprintf(module_args, sizeof(module_args), 
            "sink_name=combined-output slaves=%s,%s sink_properties=device.description=\"Combined_Output\"", 
            virtual_sink, default_sink);
    
    op = pa_context_load_module(pa->context, "module-combine-sink", module_args, NULL, NULL);
    if (op) {
        pa_operation_unref(op);
        sleep(1);
        return 1;  
    } else {
        fprintf(stderr, "Failed to create combined sink\n");
        return 0; 
    }
}

void play_audio(const char *file_name, const char *sink_name, bool hear_audio, bool loop_playback) {
    PAConnection pa = { NULL, NULL, NULL, 0 };
    
    if (!initialize_pa(&pa)) {
        return;
    }
    
    ensure_virtual_sink(&pa, sink_name);
    
    char *target_sink = strdup(sink_name);
    
    if (hear_audio) {
        char *default_sink = get_default_sink(&pa);
        if (default_sink) {
            printf("Default sink: %s\n", default_sink);
            
            if (create_combined_sink(&pa, sink_name, default_sink)) {
                free(target_sink);
                target_sink = strdup("combined-output");
            }
            
            free(default_sink);
        }
    }
    
    printf("Playing %s to %s\n", file_name, target_sink);
    
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(target_sink);
        cleanup_pa(&pa);
        return;
    }
    
    do {
        pid_t pid = fork();
        
        if (pid == -1) {
            perror("fork");
            close(pipefd[0]);
            close(pipefd[1]);
            free(target_sink);
            cleanup_pa(&pa);
            return;
        }
        
        if (pid == 0) {
            close(pipefd[0]); 
            
            execlp("ffmpeg", "ffmpeg", "-re", "-i", file_name, "-f", "pulse", "-ac", "2", target_sink, NULL);
            
          
            perror("execlp");
            exit(EXIT_FAILURE);
        }
        
        close(pipefd[1]); 
        
        int status;
        waitpid(pid, &status, 0);
        
        if (loop_playback) {
            printf("Restarting playback...\n");
        } else {
            printf("Playback complete\n");
        }
        
    } while (loop_playback);
    
    free(target_sink);
    
    cleanup_pa(&pa);
}

int main() {
    char audio_file[MAX_PATH_LENGTH];
    char sink_name[MAX_PATH_LENGTH];
    char response[10];
    bool hear_audio = false;
    bool loop_playback = false;
    
    printf("Input audio file path: ");
    if (fgets(audio_file, MAX_PATH_LENGTH, stdin) == NULL) {
        fprintf(stderr, "Error reading input\n");
        return 1;
    }
    
    audio_file[strcspn(audio_file, "\n")] = 0;
    
    if (access(audio_file, R_OK) != 0) {
        fprintf(stderr, "Error: Cannot access audio file '%s'\n", audio_file);
        perror("Reason");
        return 1;
    }
    
    printf("Input sink name (or press Enter for default 'virtual-mic'): ");
    if (fgets(sink_name, MAX_PATH_LENGTH, stdin) == NULL) {
        fprintf(stderr, "Error reading input\n");
        return 1;
    }
    
    sink_name[strcspn(sink_name, "\n")] = 0;
    
    if (strlen(sink_name) == 0) {
        strcpy(sink_name, "virtual-mic");
    }
    
    printf("Do you want to hear the audio while it plays? (y/n): ");
    if (fgets(response, sizeof(response), stdin) != NULL) {
        response[strcspn(response, "\n")] = 0;
        hear_audio = (response[0] == 'y' || response[0] == 'Y');
    }
    
    printf("Loop playback? (y/n): ");
    if (fgets(response, sizeof(response), stdin) != NULL) {
        response[strcspn(response, "\n")] = 0;
        loop_playback = (response[0] == 'y' || response[0] == 'Y');
    }
    
    printf("Playing audio file: %s\n", audio_file);
    printf("Using sink name: %s\n", sink_name);
    printf("Hear audio: %s\n", hear_audio ? "Yes" : "No");
    printf("Loop playback: %s\n", loop_playback ? "Yes" : "No");
    
    play_audio(audio_file, sink_name, hear_audio, loop_playback);
    
    return 0;
}
