#include "mupdf/fitz/fitz.h"
#include "mupdf/pdf/mupdf.h"

int open_pdf(fz_context *context, char *filename) {
    fz_stream *file;
    fz_document *doc;
    fz_page *page;
    fz_pixmap *image;
    fz_device *dev;
    fz_rect bounds;
    fz_bbox bbox;
    fz_matrix ctm;
    int faulty;
    int i, res;
    char * s;

    faulty = 0;

    /* Resolution */
    res = (int)(72 * 1.5);

    printf("Opening: %s\n", filename);

    fz_try(context) {
        file = fz_open_file(context, filename);

        doc = (fz_document *) pdf_open_document_with_stream(file);

        /* TODO Password */

        fz_close(file);
    } fz_catch (context) {
        fprintf(stderr, "Cannot open: %s\n", filename);
        faulty = 1;
    }

    if (faulty)
        return faulty;

    ctm = fz_scale(res / 72.0f, res / 72.0f);

    for(i = 0; i < fz_count_pages(doc); i++) {
        printf("Rendering page %d\n", i);
        page = fz_load_page(doc, i);

        bounds = fz_bound_page(doc, page);
        bounds.x1 *= res / 72.0f;
        bounds.y1 *= res / 72.0f;
        bbox = fz_round_rect(bounds);

        image = fz_new_pixmap_with_bbox(context, fz_device_gray, bbox);
        dev = fz_new_draw_device(context, image);

        fz_clear_pixmap_with_value(context, image, 255);
        fz_run_page(doc, page, dev, ctm, NULL);

        /* Draw onto pixmap here */

        s = malloc(sizeof(char) * 20);
        sprintf(s, "/tmp/test%d.png", i);

        printf("Saving page %d to %s\n", i, s);
        fz_write_png(context, image, s, 0);
        free(s);

        fz_drop_pixmap(context, image);

        fz_free_device(dev);

        fz_free_page(doc, page);
    }


    fz_close_document(doc);


    printf("Done opening\n");
    return 0;
}

int main (int argc, char **argv) {
    fz_context *context;

    context = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!context)
        fprintf(stderr, "Failed to create context\n");

    if (argc == 2)
        open_pdf(context, argv[1]);


    fz_free_context(context);

    return 0;
}
