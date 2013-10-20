#include <SDL/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <sys/types.h>
#include <unistd.h>
#include <math.h>

static float
    w, h,           /* Window dimensions globals */
    lw, lh,         /* Locked window dimension globals (texture generation) */
    gl_w, gl_h;     /* GL Backbuffer dimensions globals */

static SDL_Surface *surface;

static float imw, imh, ims;

/* PDF rendering */
static int pixmap_to_texture(void *pixmap, int width, int height, int format, int type);
static int page_to_texture(fz_context *ctx, fz_document *doc, int pagenum);
static void draw_screen(void);

static void toggle_fullscreen(void);

struct least_thread;
static void finish_page_render(struct least_thread *render);

/* Scrolling */
static float scroll = 0.0f;
static int autoscroll = 0;
static int autoscroll_var = 1;

/* Window settings */
static int redraw = 1; /* Window dirty? */

static int fullscreen = 0; /* Are fullscreen? */
static int prev_w, prev_h; /* W, H before fullscreen */

/* Input settings */
static int mouse_button_down = 0; /* Contains what mouse buttons are down */

#define LEAST_KEY_DOWN 1 << 1
#define LEAST_KEY_UP 1 << 2
static int key_button_down = 0; /* Contains what special keys are down */

/* Set to 1 if the machine does not support NPOT textures */
static int power_of_two = 0;

/* Set to 1 to force use of POT mechanism */
static const int force_power_of_two = 0;

/* Set to non-zero value to force render threads to specific number */
static const int force_thread_count = 1;
static int thread_count = 0;

/* Global PDF document */
static fz_document *doc;

struct least_page_info {
    int w, h, sw, sh;
    int rendering; /* Set to 1 if a thread is processing this page */
    GLuint texture;
};

/* PDF page info */
static unsigned int pagec;
static struct least_page_info *pages;

/* Cache settings */
static const int pages_to_cache = 5;
static int page_focus = 0;

/* Cache busy texture */
static GLuint busy_texture;

/* Least page render complete event */
#define LEAST_PAGE_COMPLETE (SDL_USEREVENT + 1)

/* 'base_context' lock, see explanation in least_thread structure */
static SDL_mutex *big_fitz_lock = NULL;

/* Every thread is tracked by this structure */
struct least_thread {
    /* Communication synchronisation */
    SDL_cond *cond;
    SDL_mutex *mutex;

    /* Thread ID */
    SDL_Thread *handle;
    Uint32 tid;
    int id;

    /* Fitz base_context and context.
     * 'context' is cloned from 'base_context' upon thread entry
     * 'base_context' is used for all non-parallel Fitz operation.
     */
    fz_context
        *base_context,
        *context;

    /* Pre-refresh flag */
    int pre_refresh;

    /* Action specification */
    volatile int keep_running;
    volatile int pagenum;
    volatile float scale;

    /* Action results */
    volatile fz_pixmap *pixmap;
};

static struct least_thread *threads;
static struct least_thread **idle_threads;
static int idle_thread_count;

/* Fitz lock support */
static SDL_mutex *least_lock_list[FZ_LOCK_MAX];

static void least_lock(void *user, int lock) {
    int err;
    SDL_mutex **m = user;
    err = SDL_mutexP(m[lock]);
    if (err) {
        fprintf(stderr, "During fitz lock %d, SDL error occurred: %s\n", lock,
            SDL_GetError());
    }
}

static void least_unlock(void *user, int lock) {
    int err;
    SDL_mutex **m = user;
    err = SDL_mutexV(m[lock]);
    if (err) {
        fprintf(stderr, "During fitz lock %d, SDL error occurred: %s\n", lock,
            SDL_GetError());
    }
}

static struct fz_locks_context_s least_context_locks = {
    least_lock_list,
    least_lock,
    least_unlock
};


/* Page visibility */
int inrange(float s, float e, float p) {
    float ss, ee;
    /* I know, right */
    if (s > e) {
        ss = e;
        ee = s;
    } else {
        ss = s;
        ee = e;
    }

    return p > ss && p < ee;
}

int visible_pages(int * pageinfo) {
    unsigned int i, a;
    float f, pf, s, e, scale;
    f = pf = 0;
    s = e = 0;
    pageinfo = NULL;
    a = 0;

    for(i = 0; i < pagec; i++) {
        scale = (((float)pages[i].sw / pages[i].w) * pages[i].w) / w;

        pf = f;
        f -= pages[i].sh;

        s = scroll;
        e = scroll - h * scale;

        if (
            inrange(s, e, pf) ||
            inrange(s, e, f)  ||
            inrange(pf, f, s) ||
            inrange(pf, f, e)
        ) {
            pageinfo = realloc(pageinfo, sizeof(int) * ++a);
            pageinfo[a - 1] = i;
        }
    }

    return a;
}

int open_pdf(fz_context *context, char *filename) {
    fz_stream *file;
    int faulty;
    unsigned int i;
    /* char * s; */

    faulty = 0;

    /* Resolution */

    printf("Opening: %s\n", filename);

    fz_try(context) {
        file = fz_open_file(context, filename);

        doc = (fz_document *) pdf_open_document_with_stream(context, file);

        /* TODO Password */

        fz_close(file);
    } fz_catch (context) {
        fprintf(stderr, "Cannot open: %s\n", filename);
        faulty = 1;
    }

    if (faulty)
        return faulty;

    /* XXX need error handling */
    pagec = fz_count_pages(doc);
    pages = malloc(sizeof(struct least_page_info) * pagec);

    #if 0
    return 0;
    #endif

    for(i = 0; i < pagec; i++) {
        pages[i].rendering = 0;
        pages[i].texture = 0;
        /* page_to_texture(context, doc, i); */
    }
    page_to_texture(context, doc, 0);

    printf("Done opening\n");
    return 0;
}

/* This function renders the given PDF page and returns it as a pixmap
 *
 * This code is reentrant given 'thread_context' is not currently in use
 * in any other thread. If used in a non-multithreaded way, set 'thread_context'
 * to the same value as 'context'.
 */
static fz_pixmap *page_to_pixmap(fz_context *context,
        fz_context *thread_context, fz_document *doc, int pagenum) {
    fz_page *page;
    fz_display_list *list;
    fz_pixmap *image;
    fz_device *dev;
    fz_rect bounds;
    fz_irect bbox;
    fz_matrix ctm;
    fz_colorspace *cspace;
    float scale;

    printf("Rendering page %d\n", pagenum);

    /* Now follows a bit of non-reentrant code
     * protected by the Big Fitz Lock
     *
     * XXX: Introduce mutex error checking?
     */
    SDL_mutexP(big_fitz_lock);
    {
        page = fz_load_page(doc, pagenum);

        fz_bound_page(doc, page, &bounds);

        /* XXX: There is a small risk of lw/lh being incorrect
         * due to a race condition during a refresh.
         * This shouldn't affect any visible pages though, as it causes renders
         * that will be discarded by finish_page_render to be faulty.
         */
        ims = scale = lw / bounds.x1;
        printf("W, H: (%f, %f)\n", lw, lh);
        printf("Scale: %f\n", scale);

        fz_scale(&ctm, scale, scale);

        pages[pagenum].w = bounds.x1;
        pages[pagenum].h = bounds.y1;

        bounds.x1 *= scale;
        bounds.y1 *= scale;

        pages[pagenum].sw = bounds.x1;
        pages[pagenum].sh = bounds.y1;

        fz_round_rect(&bbox, &bounds);
        printf("Size: (%d, %d)\n", bbox.x1, bbox.y1);


        list = fz_new_display_list(context);
        cspace = fz_device_rgb(context);
        image = fz_new_pixmap_with_bbox(context, cspace, &bbox);
        dev = fz_new_list_device(context, list);
        /* fz_run_page(doc, page, dev, ctm, NULL); */
        fz_run_page(doc, page, dev, &fz_identity, NULL);
        fz_free_device(dev);
    }
    SDL_mutexV(big_fitz_lock);

    /* Perform actual drawing in parallel */
    dev = fz_new_draw_device(thread_context, image);
    fz_clear_pixmap_with_value(thread_context, image, 255);

    /* XXX: Before mupdf >=1.2 it was:
     * fz_run_display_list(list, dev, &ctm, &bbox, NULL);*/
    fz_run_display_list(list, dev, &ctm, &bounds, NULL);

    fz_free_device(dev);

    /* Since some allocating was done using the main context
     * we should also deallocate using the main context.
     * At least this seems to be the case looking at
     * MuPDFs multithreading example.
     */
    SDL_mutexP(big_fitz_lock);
    {
        fz_drop_display_list(context, list);
        fz_free_page(doc, page);
    }
    SDL_mutexV(big_fitz_lock);

    return image;
}

static int page_to_texture(fz_context *context, fz_document *doc, int pagenum) {
    fz_pixmap *image;

    /* Since this function is only called initially, this is an excellent place
     * to lock the window height/width.
     * Afterwards this can be changed using the 'F5' key.
     */
    lw = w;
    lh = h;

    /* Convert page to pixmap */
    image = page_to_pixmap(context, context, doc, pagenum);

    /* Convert to texture here */
    pages[pagenum].texture = pixmap_to_texture((void*)fz_pixmap_samples(context, image),
            fz_pixmap_width(context, image),
            fz_pixmap_height(context, image), 0, 0);

    fz_drop_pixmap(context, image);

    return pages[pagenum].texture;
}


#if 0
#define DEBUG_GL(STR) \
    printf("OpenGL error " #STR ": %s\n", gluErrorString(glGetError()))
#else
#define DEBUG_GL(STR)
#endif

/* Get next power of 2 */
#define NPOW2(D, S) \
    { \
        D = S; \
        D |= D >> 1; \
        D |= D >> 2; \
        D |= D >> 4; \
        D |= D >> 8; \
        D |= D >> 16; \
        D++; \
    }

/* Round up to power of 2 */
#define RPOW2(D, S) \
    { \
        NPOW2(D, S); \
        D = D == S << 1 ? S : D; \
    }

static int pixmap_to_texture(void *pixmap, int width, int height, int format, int type)
{
    unsigned int texname;
    /* int max_texsize; */

    (void)format;
    (void)type;

    int pow2_width, pow2_height;

    /* Compute POT texture dimensions */
    RPOW2(pow2_width, width);
    RPOW2(pow2_height, height);

    format = 42;
    type = 31337;

    glGenTextures(1, &texname);
    printf("Generated texture: %d\n", texname);
    DEBUG_GL(glGenTextures);

    /* Bind the texture object */
    glBindTexture(GL_TEXTURE_2D, texname);
    DEBUG_GL(glBindTexture);

    /* Set the texture's stretching properties */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            GL_LINEAR);
    DEBUG_GL(glTexParameteri);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
            GL_LINEAR);
    DEBUG_GL(glTexParameteri);

    /* Edit the texture object's image data using the information SDL_Surface gives us */
    /*
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texsize);
    printf("Max tex dimensions: %dx%d\n", max_texsize, max_texsize);
    printf("width: %d, heigth %d\n", width, height);
    */

    /* Special treatment is only needed if the GPU does not support NPOT
     * textures and the current pixmap is not of POT dimensions.
     */
    if (power_of_two && (pow2_width != width || pow2_height != height)) {
        /*printf("pow2_width: %d, pow2_heigth %d\n", pow2_width, pow2_height); */

        /* Allocate undefined POT texture */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pow2_width,
                 pow2_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
        DEBUG_GL(glTexImage2D);

        /* Now fill texture */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
            GL_RGBA, GL_UNSIGNED_BYTE, pixmap);
        DEBUG_GL(glTexSubImage2D);

    } else {
        /* NPOT, simply pass pixmap */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width,
                 height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixmap);
        DEBUG_GL(glTexImage2D);
    }

    /* XXX: This function currently sets the global page size */
    imw = width;
    imh = height;

    return texname;
}

static void quit_tutorial(int code)
{
    unsigned int i;

    for (i = 0; i < pagec; i++)
        glDeleteTextures(1, &pages[i].texture);

    exit(code);
}

static void handle_key_up(SDL_keysym * keysym) {
    switch (keysym->sym) {
        case SDLK_DOWN:
        case SDLK_j:
            key_button_down &= ~(LEAST_KEY_DOWN);
            break;
        case SDLK_UP:
        case SDLK_k:
            key_button_down &= ~(LEAST_KEY_UP);
            break;
        default:
            break;
    }

}

static void handle_key_down(SDL_keysym * keysym)
{
    unsigned int i;

    switch (keysym->sym) {
    case SDLK_ESCAPE:
        quit_tutorial(0);
        break;

    case SDLK_DOWN:
    case SDLK_j:
        key_button_down |= LEAST_KEY_DOWN;
        break;

    case SDLK_UP:
    case SDLK_k:
        key_button_down |= LEAST_KEY_UP;
        break;

    case SDLK_PAGEDOWN:
        scroll -= imh + 20;
        redraw = 1;
        break;

    case SDLK_PAGEUP:
        scroll += imh + 20;
        redraw = 1;
        break;

    case SDLK_HOME:
        scroll = 0;
        redraw = 1;
        break;

    case SDLK_END:
        scroll = -(imh + 20) * (pagec - 1);
        redraw = 1;
        break;

    case SDLK_F5:
        printf("refresh: Killing cache\n");

        /* Kill all stored pages */
        for (i = 0; i < pagec; i++)
            if (pages[i].texture) {
                printf("refresh: Killing page %d\n", i);
                glDeleteTextures(1, &pages[i].texture);
                pages[i].texture = 0;
            } else if (pages[i].rendering) {
                printf("refresh: Removing render flag from active page %d\n",
                    i);
                pages[i].rendering = 0;
            }

        /* To prevent running renders with old settings from
         * entering the refreshed cache, mark all threads
         * as in pre-refresh state.
         */
        for (i = 0; i < (unsigned int)thread_count; i++)
            threads[i].pre_refresh = 1;
        printf("refresh: Marked %d running threads as pre-refresh renders\n",
            thread_count - idle_thread_count);

        /* Finally update the render resolution to current window size */
        printf("refresh: Changing size lock from %.2fx%.2f to %.2fx%.2f\n",
            lw, lh, w, h);

        lw = w;
        lh = h;
        break;

    case SDLK_F11:
        SDL_WM_ToggleFullScreen(surface);
        toggle_fullscreen();
        break;

    case SDLK_F12:
        if (autoscroll)
            autoscroll = 0;
        else
            autoscroll = 1;
        redraw = 1;
        break;

    default:
        break;
    }
}

static void handle_mouse_down(SDL_MouseButtonEvent *event) {
    switch (event->button) {
        case 1:
            mouse_button_down |= 1 << 1;
            break;
        case 2:
            mouse_button_down |= 1 << 2;
            break;
        case 3:
            mouse_button_down |= 1 << 3;
            break;
        case 4:
            scroll += 100;
            redraw = 1;
            break;
        case 5:
            scroll -= 100;
            redraw = 1;
            break;
    }

    return;
}

static void handle_mouse_up(SDL_MouseButtonEvent *event) {
    switch (event->button) {
        case 1:
            mouse_button_down &= ~(1 << 1);
            break;
        case 2:
            mouse_button_down &= ~(1 << 2);
            break;
        case 3:
            mouse_button_down &= ~(1 << 3);
            break;
        case 4:
            scroll += 100;
            redraw = 1;
            break;
        case 5:
            scroll -= 100;
            redraw = 1;
            break;
    }

    return;
}

static void handle_mouse_motion(SDL_MouseMotionEvent *event) {
    if (event->state) {
        if (mouse_button_down & (1 << 3)) {
            /*
            printf("Mouse button moving and down: rel: (%d, %d)\n",
                    event->xrel, event->yrel);
                    */
            scroll += event->yrel * 2;
            redraw = 1;
        }

    }

}

static void toggle_fullscreen(void) {
    const SDL_VideoInfo *info = NULL;
    info = SDL_GetVideoInfo();

    if (!info) {
        puts("Oops - can't get video info");
    }

    if (!fullscreen) {
        fullscreen = 1;

        prev_w = w;
        prev_h = h;

        w = info->current_w;
        h = info->current_h;
    } else {
        fullscreen = 0;
        w = prev_w;
        h = prev_h;
    }


    redraw = 1;
}

static void handle_resize(SDL_ResizeEvent e) {
    /* printf("Resized to (%d, %d)\n", e.w, e.h); */
    w = e.w;
    h = e.h;

    redraw = 1;


    return;
}

static void setup_opengl(int width, int height)
{
    /* float ratio = (float)width / (float)height; */

    /* Our shading model--Gouraud (smooth). */
    glShadeModel(GL_SMOOTH);

    glEnable(GL_TEXTURE_2D);

    /* Set the clear color. */
    glClearColor(0, 0, 0, 0);

    /* Setup our viewport. */
    glViewport(0, 0, width, height);

    glLoadIdentity();
}

int setup_sdl(void)
{
    /* Information about the current video settings. */
    const SDL_VideoInfo *info = NULL;

    /* Color depth in bits of our window. */
    int bpp = 0;

    /* Flags we will pass into SDL_SetVideoMode. */
    int flags = 0;

    /* First, initialize SDL's video subsystem. */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        /* Failed, exit. */
        fprintf(stderr, "Video initialisation failed: %s\n",
            SDL_GetError());
        quit_tutorial(1);
    }

    /* Let's get some video information. */
    info = SDL_GetVideoInfo();

    if (!info) {
        /* This should probably never happen. */
        fprintf(stderr, "Video query failed: %s\n", SDL_GetError());
        quit_tutorial(1);
    }

    /* Store current width and height, and more importantly
     * store GL backbuffer size
     */
    gl_w = w = info->current_w;
    gl_h = h = info->current_h;
    printf("W, H: (%f, %f)\n", w, h);

    bpp = info->vfmt->BitsPerPixel;

    /*
     * Now, we want to setup our requested
     * window attributes for our OpenGL window.
     * We want *at least* 5 bits of red, green
     * and blue. We also want at least a 16-bit
     * depth buffer.
     *
     * The last thing we do is request a double
     * buffered window. '1' turns on double
     * buffering, '0' turns it off.
     *
     * Note that we do not use SDL_DOUBLEBUF in
     * the flags to SDL_SetVideoMode. That does
     * not affect the GL attribute state, only
     * the standard 2D blitting setup.
     */
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* flags = SDL_OPENGL | SDL_FULLSCREEN; */
    flags = SDL_OPENGL | SDL_RESIZABLE | SDL_DOUBLEBUF;
    /* flags = SDL_OPENGL; */
    surface = SDL_SetVideoMode(w, h, bpp, flags);
    if (!surface) {
        /*
         * This could happen for a variety of reasons,
         * including DISPLAY not being set, the specified
         * resolution not being available, etc.
         */
        fprintf(stderr, "Video mode set failed: %s\n", SDL_GetError());
        quit_tutorial(1);
    }


    SDL_WM_SetCaption("least", "least");

    return 0;
}

static void process_events(void)
{
    /* Our SDL event placeholder. */
    SDL_Event event;

    /* Only poll + sleep if we are autoscrolling or doing
     * something else that is interactive */
    if (((autoscroll) && autoscroll_var) || key_button_down) {
        if (!SDL_PollEvent(&event)) {
            /* If we add a sleep, the scrolling won't be super smooth.
             * Regardless, I think we need to find something to make sure we
             * don't eat 100% cpu just checking for events.
             *
             * I found that 10ms is not a bad wait. Theoretically we want to
             * wait 1000ms / fps (usually 60) -> 16ms.
             * */
            usleep(16000);

            if (autoscroll) {
                if (key_button_down & LEAST_KEY_DOWN)
                    autoscroll_var += 1;

                if (key_button_down & LEAST_KEY_UP)
                    autoscroll_var -= 1;

                scroll -= autoscroll_var;
            } else {
                if (key_button_down & LEAST_KEY_DOWN)
                    scroll -= 5;

                if (key_button_down & LEAST_KEY_UP)
                    scroll += 5;
            }

            redraw = 1;
        }
    } else {
        SDL_WaitEvent(&event);
    }

next_event:

    switch (event.type) {
    case SDL_KEYDOWN:
        /* Handle key presses. */
        handle_key_down(&event.key.keysym);
        break;

    case SDL_KEYUP:
        handle_key_up(&event.key.keysym);
        break;

    case SDL_QUIT:
        /* Handle quit requests (like Ctrl-c). */
        quit_tutorial(0);
        break;

    case SDL_VIDEORESIZE:
        handle_resize(event.resize);
        break;

    case SDL_VIDEOEXPOSE:
        redraw = 1;
        break;

    case SDL_MOUSEBUTTONDOWN:
        handle_mouse_down(&event.button);
        break;

    case SDL_MOUSEBUTTONUP:
        handle_mouse_up(&event.button);
        break;

    case SDL_MOUSEMOTION:
        handle_mouse_motion(&event.motion);
        break;

    /* A thread completed its rendering
     *
     * The thread structure of the completed job is contained
     * within the data1 pointer of the event.
     */
    case LEAST_PAGE_COMPLETE:
        finish_page_render((struct least_thread*)event.user.data1);
        redraw = 1;
        break;

    }

    /* Clear event, just in case SDL doesn't do this (TODO) */
    memset(&event, 0, sizeof(SDL_Event));
    /* If there are more events, handle them before drawing.
     * This is required for scrolling with the mouse - without this,
     * it is pretty slow and lags. */
    if (SDL_PollEvent(&event)) {
        goto next_event;
    }
}

/* Render thread entry */
static int render_thread(void *t)
{
    SDL_Event my_event;
    struct least_thread *self = t;

    my_event.type = LEAST_PAGE_COMPLETE;
    my_event.user.data1 = t;

    /* Store local SDL thread ID (Is this function thread safe? ;-) */
    self->tid = SDL_ThreadID();

    self->context = fz_clone_context(self->base_context);
    if (!self->context) {
        fprintf(stderr, "In render thread %d: fz_clone_context returned NULL\n",
            self->id);
        abort();
    }

    printf("Render thread %d up and running.\n", self->id);
    SDL_mutexP(self->mutex);
    /* Wait for first command */
    SDL_CondWait(self->cond, self->mutex);

    while (self->keep_running) {

        printf("Thread %d: Rendering page %d\n", self->id, self->pagenum);

        /* Render a page */
        self->pixmap = page_to_pixmap(self->base_context, self->context, doc, self->pagenum);
        if (!self->pixmap) {
            fprintf(stderr, "In render thread %d: "
                "page_to_pixmap returned NULL\n", self->id);
            abort();
        }

        /* Push completed page to event queue */
        SDL_PushEvent(&my_event);

        SDL_CondWait(self->cond, self->mutex);
    }

    SDL_mutexV(self->mutex);

    /* Cleanup */
    fz_free_context(self->context);

    return 0;
}

static void draw_screen(void)
{
    unsigned int i;
    unsigned int
        page_offset,
        pages_rendered;
    int ww, hh;
    int pow2_ww, pow2_hh;
    float tsm, ttm, tsc, ttc;

    /* View dimensions of pages */
    float vw, vh;
    /* static float vloot = 0.f; */

    ww = imw;
    hh = imh;

    glEnable(GL_TEXTURE_2D);

    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();

    /* Setup texture matrix to certain scale whenever pow2 is required */
    if (power_of_two) {
        RPOW2(pow2_ww, ww);
        RPOW2(pow2_hh, hh);
        glScalef(ww / (float)pow2_ww, hh / (float)pow2_hh, 1.0f);
        ttm = (float)pow2_hh / hh * 8;
        tsm = (float)pow2_ww / ww * 8;
    } else {
        tsm = 8;
        ttm = 8;
    }

    glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
    glViewport(0, 0, (int)w, (int)gl_h);
    /* glViewport(0, 0, 400, 400); */
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0f, (int)w, (int)gl_h, 0, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /*
    vloot += 0.1;
    glRotatef(vloot, 0.f, 0.f, 1.0f);
    */

    /* Compute pageview dimensions */
    vw = w;
    vh = (vw / ww) * hh;

    pages_rendered = h / (vh + (vh / imh) * 20) + 2;

    if (scroll < (imh + 20) * pages_rendered) {
        /* Scale based scroll */
        if (scroll > 0)
            glTranslatef(0.f, scroll, 0.f);
        else
            glTranslatef(0.f, fmod(scroll, imh + 20) * (vh / imh) , 0.f);
        page_offset = fmax(-scroll / (float)(imh + 20), 0);
        /*printf("page_offset: %d\n", page_offset); */
        /*printf("pages_rendered: %d\n", pages_rendered);*/

        glColor3f(1.0, 1.0, 1.0);
        for (i = page_offset; i < page_offset + pages_rendered &&
                i < pagec; i++) {
            /* printf("Page: %d, size: (%f, %f)\n", i, imw, imh); */

            /* printf("Binding texture: %d\n", pages[i].texture); */
            if (pages[i].texture) {
                /* printf("Binding texture: %d\n", pages[i].texture); */
                glBindTexture(GL_TEXTURE_2D, pages[i].texture);
                tsc = ttc = 1;
            } else {
                /* puts("Binding busy"); */
                glBindTexture(GL_TEXTURE_2D, busy_texture);
                tsc = tsm;
                ttc = ttm;
            }
            /* printf("OpenGL error: %s\n", gluErrorString(glGetError())); */
            /* Send our triangle data to the pipeline. */
            glBegin(GL_QUADS);

            /* Bottom-left vertex (corner) */
            glTexCoord2f(0, 0);
            glVertex3f(0.f, 0.f, 0.0f);

            /* Bottom-right vertex (corner) */
            glTexCoord2f(tsc, 0);
            glVertex3f(vw, 0.f, 0.f);

            /* Top-right vertex (corner) */
            glTexCoord2f(tsc, ttc);
            glVertex3f(vw, vh, 0.f);

            /* Top-left vertex (corner) */
            glTexCoord2f(0, ttc);
            glVertex3f(0.f, vh, 0.f);

            glEnd();
            /*
            if (i % 2 == 0)
                glTranslatef(ww + 20, 0.f, 0.f);
            else {
                glTranslatef(-ww -20, hh + 20, 0.f);
            }
            */
            glTranslatef(0.0f, vh + (vh / imh) * 20, 0.f);
        }
    }

    /*
     * Swap the buffers. This this tells the driver to
     * render the next frame from the contents of the
     * back-buffer, and to set all rendering operations
     * to occur on what was the front-buffer.
     *
     * Double buffering prevents nasty visual tearing
     * from the application drawing on areas of the
     * screen that are being updated at the same time.
     */
    SDL_GL_SwapBuffers();
}

/* XXX Error handling :-( */
static void init_busy_texture() {
    unsigned int tex[4] =
        {
            0xffaa8888,
            0xff554444,
            0xff554444,
            0xffaa8888
        };

    glGenTextures(1, &busy_texture);
    printf("Busy texture @ num: %d\n", busy_texture);
    glBindTexture(GL_TEXTURE_2D, busy_texture);
    DEBUG_GL(glBindTexture);

    /* Set the texture's stretching properties */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    DEBUG_GL(glTexParameteri);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    DEBUG_GL(glTexParameteri);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            GL_NEAREST);
    DEBUG_GL(glTexParameteri);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
            GL_NEAREST);
    DEBUG_GL(glTexParameteri);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2,
        0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    DEBUG_GL(glTexImage2D);
}

static void init_threads(int thread_count, fz_context *context) {
    int i;

#if 0 /* Unfortunately SDL does not support changing of thread stack size.
       * Let's have some faith in their default settings then. ;-)
       */

    /* Fetch current stacksize */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stack_size);
    printf("Default stack size: %zu\n", stack_size);

    /* Ensure at least 1MB of stacksize */
    if (stack_size < 1048576) {
        stack_size = 1048576;
        printf("Changing to %zu\n", stack_size);
        pthread_attr_setstacksize(&attr, stack_size);
    } else {
        puts("This stack size is okay.");
    }
#endif

    threads = malloc(sizeof(struct least_thread) * thread_count);
    idle_threads = malloc(sizeof(struct least_thread*) * thread_count);

    for (i = 0; i < thread_count; i++) {
        /* Setup sync */
        threads[i].cond  = SDL_CreateCond();
        if (!threads[i].cond) {
            fprintf(stderr, "Creating condition failed: %s\n", SDL_GetError());
            abort();
        }
        threads[i].mutex = SDL_CreateMutex();
        if (!threads[i].mutex) {
            fprintf(stderr, "Creating mutex failed: %s\n", SDL_GetError());
            abort();
        }


        /* Thread ID */
        threads[i].id = i;

        /* Setup loop and context */
        threads[i].context = NULL;
        threads[i].base_context = context;
        threads[i].keep_running = 1;

        #if 0
        threads[i].context = fz_clone_context(context);
        if (!threads[i].context) {
            fprintf(stderr, "While creating thread %d: fz_clone_context "
                "returned NULL\n", threads[i].id);
            abort();
        }
        #endif

        threads[i].handle = SDL_CreateThread(render_thread,
            (void*)(threads + i));
        if (!threads[i].handle) {
            fprintf(stderr, "Creating thread failed: %s\n", SDL_GetError());
            abort();
        }

        idle_threads[thread_count - i - 1] = threads + i;
    }

    idle_thread_count = thread_count;

    return;
}

/* Initialises mutexes required for Fitz locking */
static void init_least_context_locks(void)
{
    int i;

    /* Initialise the Big Fitz Lock */
    big_fitz_lock = SDL_CreateMutex();
    if (!big_fitz_lock) {
        fprintf(stderr, "Mutex initialisation failed: %s\n",
            SDL_GetError());
        abort();
    }

    for (i = 0; i < FZ_LOCK_MAX; i++) {
        least_lock_list[i] = SDL_CreateMutex();
        if (!least_lock_list[i]) {
            fprintf(stderr, "Mutex initialisation failed: %s\n",
                SDL_GetError());
            abort();
        }
    }

    return;
}

static void schedule_page(int pagenum)
{
    struct least_thread *t = idle_threads[--idle_thread_count];

    /* Mark page in progress */
    pages[pagenum].rendering = 1;

    /* Configure thread */
    t->pagenum = pagenum;
    t->scale = ims;
    t->pre_refresh = 0;

    /* Start rendering */
    SDL_mutexP(t->mutex);
    SDL_CondSignal(t->cond);
    SDL_mutexV(t->mutex);

    return;
}

/* This function updates cache state if necessary
 *
 * It schedules render jobs en removes pages no longer
 * needed.
 *
 * The cache currently uses the scroll variable for computing
 * the focus page.
 */
void update_cache(void)
{
    int i;
    int
        c_start,
        c_stop;
    int kills_left = idle_thread_count;

    /* Compute page_focus */
    if (scroll > 0.) {
        page_focus = 0;
    } else {
        /* Page focus should be on the page occupying most of the display
         *
         * Every page takes up imh + 20 units of space in between.
         * Split the window in 2 to scroll to the middle of the window.
         * Finally add 10 units of scroll, because only 10 of the 20 units
         * space belong the page on top of the window. This should create
         * satisfying focus behaviour.
         */
        page_focus = (-scroll + (h / 2) + 10) / (imh + 20);
        if (page_focus >= (int)pagec)
            page_focus = pagec - 1;
    }

    /* Compute sliding cache window */
    c_start = page_focus - (pages_to_cache - 1) / 2;
    if (c_start < 0)
        c_start = 0;

    c_stop = c_start + pages_to_cache;
    if (c_stop > (int)pagec) {
        c_stop = pagec;
        c_start = c_stop - pages_to_cache;
        if (c_start < 0)
            c_start = 0;
    }

#if 0
    printf("Page focus is: %d\n", page_focus);
    printf("Current cache window: [%d, %d)\n", c_start, c_stop);
    printf("Idle thread count: %d\n", idle_thread_count);
#endif

    /* First kill unnecessary pages in cache */
    for (i = 0; i < c_start && kills_left; i++) {
        if (pages[i].texture) {
            printf("cache: Killing page %d\n", i);
            glDeleteTextures(1, &pages[i].texture);
            pages[i].texture = 0;
            kills_left--;
        }
    }

    for (i = c_stop; i < (int)pagec && kills_left; i++) {
        if (pages[i].texture) {
            printf("cache: Killing page %d\n", i);
            glDeleteTextures(1, &pages[i].texture);
            pages[i].texture = 0;
            kills_left--;
        }
    }

    /* Schedule new pages */
    for (i = c_start; i < c_stop && idle_thread_count; i++) {
        if (!pages[i].texture && !pages[i].rendering) {
            printf("cache: Scheduling page %d\n", i);
            schedule_page(i);
        }
    }
}

/* This function completes a rendering job.
 *
 * The thread is added to the pool of idle threads.
 */
static void finish_page_render(struct least_thread *render)
{
    union _dispose_volatile {
        volatile fz_pixmap *volatile_pixmap;
        fz_pixmap *pixmap;
    } d;

    d.volatile_pixmap = render->pixmap;

    /* XXX Error handling ? */
    if (render->pre_refresh)
        printf("finish_page: Discarding pre-refresh render "
            "of page %d by thread %d\n", render->pagenum, render->id);
    else {
        /* Page is complete and no longer rendering */
        pages[render->pagenum].rendering = 0;

        /* Convert to texture */
        pages[render->pagenum].texture = pixmap_to_texture(
            (void*)fz_pixmap_samples(render->context, d.pixmap),
            fz_pixmap_width(render->context, d.pixmap),
            fz_pixmap_height(render->context, d.pixmap), 0, 0);
    }

    /* XXX Using the threads context might not be a gr8 idea */
    fz_drop_pixmap(render->context, d.pixmap);

    /* Place thread into idle pool */
    idle_threads[idle_thread_count++] = render;
}

int main (int argc, char **argv) {
    fz_context *context;
    int *pageinfo = NULL;
    /* int i; */

    /* Initialises mutexes required for Fitz locking */
    init_least_context_locks();

    context = fz_new_context(NULL, &least_context_locks, FZ_STORE_DEFAULT);
    if (!context)
        fprintf(stderr, "Failed to create context\n");

    if (force_thread_count)
        thread_count = force_thread_count;
    else
        thread_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (argc == 2) {
        /* Initialize OpenGL window */
        setup_sdl();

        /* Start render threads */
        init_threads(thread_count, context);

        /*
         * At this point, we should have a properly setup
         * double-buffered window for use with OpenGL.
         */
        setup_opengl(w, h);
        init_busy_texture();

        /* Check for non-power-of-two support */
        /* printf("Extensions are: %s\n", glGetString(GL_EXTENSIONS)); */
        if (strstr((const char *)glGetString(GL_EXTENSIONS),
            "GL_ARB_texture_non_power_of_two")) {
            puts("Machine supports NPOT textures.");
            power_of_two = 0;
        } else {
            puts("Machine supports POT textures only.");
            power_of_two = 1;
        }

        power_of_two |= force_power_of_two;

        /* Load textures from PDF file */
        open_pdf(context, argv[1]);

        /*
         * Now we want to begin our normal app process--
         * an event loop with a lot of redrawing.
         */

        while (1) {
            /* Process incoming events. */
            process_events();

            /* Update cache state */
            update_cache();

            if (redraw) {
                redraw = 0;
                draw_screen();
            }

            free(pageinfo);
        }


    }


    fz_close_document(doc);
    fz_free_context(context);

    return 0;
}
