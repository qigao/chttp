#include <openapi/generator.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    fputs("Usage: openapi-gen --plugin MODULE [--format json|yaml] [--title TITLE]\n"
          "                   [--version VERSION] [--output FILE] SOURCE.c [...]\n", stderr);
}

int main(int argc, char **argv) {
    const char *plugin_path = NULL, *format = "json", *title = "API", *version = "1.0.0", *output = NULL;
    int first_source = argc;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (argv[i][0] != '-') { first_source = i; break; }
        if (!strcmp(argv[i], "--")) { first_source = i + 1; break; }
        if (i + 1 == argc) { usage(); return 2; }
        const char *option = argv[i++], *value = argv[i];
        if (!strcmp(option, "--plugin")) plugin_path = value;
        else if (!strcmp(option, "--format")) format = value;
        else if (!strcmp(option, "--title")) title = value;
        else if (!strcmp(option, "--version")) version = value;
        else if (!strcmp(option, "--output")) output = value;
        else { usage(); return 2; }
    }
    if (!plugin_path || first_source == argc || (strcmp(format, "json") && strcmp(format, "yaml"))) { usage(); return 2; }
    oa_error error = {{0}};
    oa_plugin *plugin = oa_plugin_open(plugin_path, &error);
    oa_document *document = NULL;
    char *text = NULL;
    int status = 1;
    if (!plugin) goto cleanup;
    document = oa_document_create(title, version, &error);
    if (!document) goto cleanup;
    for (int i = first_source; i < argc; ++i) {
        FILE *file = fopen(argv[i], "rb");
        if (!file) { snprintf(error.message, sizeof(error.message), "cannot open source: %s", argv[i]); goto cleanup; }
        if (fseek(file, 0, SEEK_END)) { fclose(file); snprintf(error.message, sizeof(error.message), "cannot seek source"); goto cleanup; }
        long size = ftell(file);
        if (size < 0 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
            fclose(file); snprintf(error.message, sizeof(error.message), "source exceeds 16 MiB or cannot be read"); goto cleanup;
        }
        char *source = malloc((size_t)size + 1);
        if (!source) { fclose(file); snprintf(error.message, sizeof(error.message), "out of memory"); goto cleanup; }
        size_t read = fread(source, 1, (size_t)size, file);
        int failed = ferror(file);
        fclose(file);
        if (read != (size_t)size || failed) { free(source); snprintf(error.message, sizeof(error.message), "cannot read source"); goto cleanup; }
        int added = oa_document_add(document, plugin, source, read, &error);
        free(source);
        if (!added) { fprintf(stderr, "%s: ", argv[i]); goto cleanup; }
    }
    size_t length;
    text = oa_document_render(document, format, &length, &error);
    if (!text) goto cleanup;
    FILE *destination = output ? fopen(output, "wb") : stdout;
    if (!destination) { snprintf(error.message, sizeof(error.message), "cannot open output: %s", output); goto cleanup; }
    int write_failed = fwrite(text, 1, length, destination) != length;
    if (output) write_failed |= fclose(destination) != 0;
    else write_failed |= fflush(destination) != 0;
    if (write_failed) { snprintf(error.message, sizeof(error.message), "cannot write output"); goto cleanup; }
    status = 0;
cleanup:
    if (status) fprintf(stderr, "%s\n", error.message);
    oa_text_free(text);
    oa_document_free(document);
    oa_plugin_close(plugin);
    return status;
}
