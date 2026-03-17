#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gpiod.h>

#define PRINTER_PORT "/dev/serial0"
#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_LINE 23
#define TOTAL_FILES 20
#define MAX_PATH_LEN 512
#define TEXT_DIR "/home/be/texts"

#define DEBOUNCE_MS 500
#define HYSTERESIS_MS 1000

static long long timespec_to_ms(const struct timespec *ts) {
    return ((long long)ts->tv_sec * 1000LL) + (ts->tv_nsec / 1000000LL);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ms(&ts);
}

static char *load_random_text_file(const char *directory_path) {
    char filepath[MAX_PATH_LEN];
    int file_index = (rand() % TOTAL_FILES) + 1;

    snprintf(filepath, sizeof(filepath), "%s/%d.txt", directory_path, file_index);

    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("Failed to open text file");
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek failed");
        fclose(fp);
        return NULL;
    }

    long file_size = ftell(fp);
    if (file_size < 0) {
        perror("ftell failed");
        fclose(fp);
        return NULL;
    }

    rewind(fp);

    char *buffer = malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        perror("malloc failed");
        fclose(fp);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, fp);
    if (ferror(fp)) {
        perror("fread failed");
        free(buffer);
        fclose(fp);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    fclose(fp);

    printf("Loaded file: %s\n", filepath);
    return buffer;
}

static int print_random_file(void) {
    char *text = load_random_text_file(TEXT_DIR);
    if (text == NULL) {
        return -1;
    }

    /* ESC d 4 -> print and feed 4 lines */
    unsigned char printer_cmds[] = {0x1B, 0x64, 0x04};

    FILE *pport = fopen(PRINTER_PORT, "w");
    if (pport == NULL) {
        perror("Failed to open printer port");
        free(text);
        return -1;
    }

    if (fputs(text, pport) == EOF) {
        perror("Failed to write text to printer");
        free(text);
        fclose(pport);
        return -1;
    }

    for (size_t i = 0; i < sizeof(printer_cmds); i++) {
        if (fputc(printer_cmds[i], pport) == EOF) {
            perror("Failed to write printer command");
            free(text);
            fclose(pport);
            return -1;
        }
    }

    if (fflush(pport) != 0) {
        perror("fflush failed");
        free(text);
        fclose(pport);
        return -1;
    }

    if (fclose(pport) != 0) {
        perror("Failed to close printer port");
        free(text);
        return -1;
    }

    free(text);
    return 0;
}

int main(void) {
    struct gpiod_chip *chip = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    struct gpiod_line_request *request = NULL;
    struct gpiod_edge_event_buffer *event_buffer = NULL;
    struct gpiod_edge_event *event = NULL;

    unsigned int offset = GPIO_LINE;
    int ret;

    long long last_valid_trigger_ms = 0;
    long long ignore_until_ms = 0;

    srand((unsigned int)time(NULL));

    chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        perror("gpiod_chip_open");
        goto cleanup;
    }

    settings = gpiod_line_settings_new();
    if (!settings) {
        fprintf(stderr, "gpiod_line_settings_new failed\n");
        goto cleanup;
    }

    ret = gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    if (ret < 0) {
        perror("gpiod_line_settings_set_direction");
        goto cleanup;
    }

    ret = gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_FALLING);
    if (ret < 0) {
        perror("gpiod_line_settings_set_edge_detection");
        goto cleanup;
    }

    ret = gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    if (ret < 0) {
        perror("gpiod_line_settings_set_bias");
        goto cleanup;
    }

    line_config = gpiod_line_config_new();
    if (!line_config) {
        fprintf(stderr, "gpiod_line_config_new failed\n");
        goto cleanup;
    }

    ret = gpiod_line_config_add_line_settings(line_config, &offset, 1, settings);
    if (ret < 0) {
        perror("gpiod_line_config_add_line_settings");
        goto cleanup;
    }

    request_config = gpiod_request_config_new();
    if (!request_config) {
        fprintf(stderr, "gpiod_request_config_new failed\n");
        goto cleanup;
    }

    gpiod_request_config_set_consumer(request_config, "thermal-printer-trigger");

    request = gpiod_chip_request_lines(chip, request_config, line_config);
    if (!request) {
        perror("gpiod_chip_request_lines");
        goto cleanup;
    }

    event_buffer = gpiod_edge_event_buffer_new(1);
    if (!event_buffer) {
        fprintf(stderr, "gpiod_edge_event_buffer_new failed\n");
        goto cleanup;
    }

    printf("Waiting for falling edge on GPIO%d...\n", GPIO_LINE);

    while (1) {
        ret = gpiod_line_request_wait_edge_events(request, -1);
        if (ret < 0) {
            perror("gpiod_line_request_wait_edge_events");
            break;
        }

        if (ret == 0) {
            continue;
        }

        ret = gpiod_line_request_read_edge_events(request, event_buffer, 1);
        if (ret < 0) {
            perror("gpiod_line_request_read_edge_events");
            break;
        }

        if (ret == 0) {
            continue;
        }

        event = gpiod_edge_event_buffer_get_event(event_buffer, 0);
        if (!event) {
            fprintf(stderr, "Failed to get edge event from buffer\n");
            break;
        }

        if (gpiod_edge_event_get_event_type(event) == GPIOD_EDGE_EVENT_FALLING_EDGE) {
            long long current_ms = now_ms();

            if (current_ms < ignore_until_ms) {
                printf("Ignored: inside hysteresis window\n");
                continue;
            }

            if ((current_ms - last_valid_trigger_ms) < DEBOUNCE_MS) {
                printf("Ignored: bounce detected\n");
                continue;
            }

            printf("Falling edge detected on GPIO%d\n", GPIO_LINE);

            last_valid_trigger_ms = current_ms;
            ignore_until_ms = current_ms + HYSTERESIS_MS;

            if (print_random_file() != 0) {
                fprintf(stderr, "Print failed\n");
            }
        }
    }

cleanup:
    if (event_buffer)
        gpiod_edge_event_buffer_free(event_buffer);
    if (request)
        gpiod_line_request_release(request);
    if (request_config)
        gpiod_request_config_free(request_config);
    if (line_config)
        gpiod_line_config_free(line_config);
    if (settings)
        gpiod_line_settings_free(settings);
    if (chip)
        gpiod_chip_close(chip);

    return 0;
}
