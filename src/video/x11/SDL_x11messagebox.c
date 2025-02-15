/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2012 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_config.h"

#if SDL_VIDEO_DRIVER_X11

#include "SDL.h"
#include "SDL_x11video.h"
#include "SDL_x11dyn.h"
#include "SDL_assert.h"

#include <locale.h>


#define SDL_FORK_MESSAGEBOX 1

#if SDL_FORK_MESSAGEBOX
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#endif

#define MAX_BUTTONS             8       /* Maximum number of buttons supported */
#define MAX_TEXT_LINES          32      /* Maximum number of text lines supported */
#define MIN_BUTTON_WIDTH        64      /* Minimum button width */
#define MIN_DIALOG_WIDTH        200     /* Minimum dialog width */
#define MIN_DIALOG_HEIGHT       100     /* Minimum dialog height */

static const char g_MessageBoxFontLatin1[] = "-*-*-medium-r-normal--0-120-*-*-p-0-iso8859-1";
static const char g_MessageBoxFont[] = "-*-*-*-*-*-*-*-*-*-*-*-*-*-*";

static const SDL_MessageBoxColor g_default_colors[ SDL_MESSAGEBOX_COLOR_MAX ] = {
    { 56,  54,  53  }, // SDL_MESSAGEBOX_COLOR_BACKGROUND,
    { 209, 207, 205 }, // SDL_MESSAGEBOX_COLOR_TEXT,
    { 140, 135, 129 }, // SDL_MESSAGEBOX_COLOR_BUTTON_BORDER,
    { 105, 102, 99  }, // SDL_MESSAGEBOX_COLOR_BUTTON_BACKGROUND,
    { 205, 202, 53  }, // SDL_MESSAGEBOX_COLOR_BUTTON_SELECTED,
};

#define SDL_MAKE_RGB( _r, _g, _b )  ( ( ( Uint32 )( _r ) << 16 ) | \
                                      ( ( Uint32 )( _g ) << 8 ) |  \
                                      ( ( Uint32 )( _b ) ) )

typedef struct SDL_MessageBoxButtonDataX11 {
    int x, y;                           /* Text position */
    int length;                         /* Text length */
    int text_width;                     /* Text width */

    SDL_Rect rect;                      /* Rectangle for entire button */

    const SDL_MessageBoxButtonData *buttondata;   /* Button data from caller */
} SDL_MessageBoxButtonDataX11;

typedef struct TextLineData {
    int width;                          /* Width of this text line */
    int length;                         /* String length of this text line */
    const char *text;                   /* Text for this line */
} TextLineData;

typedef struct SDL_MessageBoxDataX11 {
    XFontSet font_set;                  /* for UTF-8 systems */
    XFontStruct *font_struct;            /* Latin1 (ASCII) fallback. */
    Window window;
    Display *display;
    long event_mask;
    Atom wm_protocols;
    Atom wm_delete_message;

    int dialog_width;                   /* Dialog box width. */
    int dialog_height;                  /* Dialog box height. */

    int xtext, ytext;                   /* Text position to start drawing at. */
    int numlines;                       /* Count of Text lines. */
    int text_height;                    /* Height for text lines. */
    TextLineData linedata[ MAX_TEXT_LINES ];

    int *pbuttonid;                     /* Pointer to user return buttonid value. */

    int button_press_index;             /* Index into buttondata/buttonpos for button which is pressed (or -1). */
    int mouse_over_index;               /* Index into buttondata/buttonpos for button mouse is over (or -1). */

    int numbuttons;                     /* Count of buttons. */
    const SDL_MessageBoxButtonData *buttondata;
    SDL_MessageBoxButtonDataX11 buttonpos[ MAX_BUTTONS ];

    Uint32 color[ SDL_MESSAGEBOX_COLOR_MAX ];

    const SDL_MessageBoxData *messageboxdata;
} SDL_MessageBoxDataX11;

/* Maximum helper for ints. */
static __inline__ int
IntMax( int a, int b )
{
    return ( a > b  ) ? a : b;
}

/* Return width and height for a string. */
static void
GetTextWidthHeight( SDL_MessageBoxDataX11 *data, const char *str, int nbytes, int *pwidth, int *pheight )
{
    if (SDL_X11_HAVE_UTF8) {
        XRectangle overall_ink, overall_logical;
        Xutf8TextExtents(data->font_set, str, nbytes, &overall_ink, &overall_logical);
        *pwidth = overall_logical.width;
        *pheight = overall_logical.height;
    } else {
        XCharStruct text_structure;
        int font_direction, font_ascent, font_descent;
        XTextExtents( data->font_struct, str, nbytes,
                      &font_direction, &font_ascent, &font_descent,
                      &text_structure );
        *pwidth = text_structure.width;
        *pheight = text_structure.ascent + text_structure.descent;
    }
}

/* Return index of button if position x,y is contained therein. */
static int
GetHitButtonIndex( SDL_MessageBoxDataX11 *data, int x, int y )
{
    int i;
    int numbuttons = data->numbuttons;
    SDL_MessageBoxButtonDataX11 *buttonpos = data->buttonpos;

    for ( i = 0; i < numbuttons; i++ ) {
        SDL_Rect *rect = &buttonpos[ i ].rect;

        if ( ( x >= rect->x ) &&
             ( x <= ( rect->x + rect->w ) ) &&
             ( y >= rect->y ) &&
             ( y <= ( rect->y + rect->h ) ) ) {
            return i;
        }
    }

    return -1;
}

/* Initialize SDL_MessageBoxData structure and Display, etc. */
static int
X11_MessageBoxInit( SDL_MessageBoxDataX11 *data, const SDL_MessageBoxData * messageboxdata, int * pbuttonid )
{
    int i;
    int numbuttons = messageboxdata->numbuttons;
    const SDL_MessageBoxButtonData *buttondata = messageboxdata->buttons;
    const SDL_MessageBoxColor *colorhints;

    if ( numbuttons > MAX_BUTTONS ) {
        SDL_SetError("Too many buttons (%d max allowed)", MAX_BUTTONS);
        return -1;
    }

    data->dialog_width = MIN_DIALOG_WIDTH;
    data->dialog_height = MIN_DIALOG_HEIGHT;
    data->messageboxdata = messageboxdata;
    data->buttondata = buttondata;
    data->numbuttons = numbuttons;
    data->pbuttonid = pbuttonid;

    data->display = XOpenDisplay( NULL );
    if ( !data->display ) {
        SDL_SetError("Couldn't open X11 display");
        return -1;
    }

    if (SDL_X11_HAVE_UTF8) {
        char **missing = NULL;
        int num_missing = 0;
        data->font_set = XCreateFontSet(data->display, g_MessageBoxFont,
                                        &missing, &num_missing, NULL);
        if ( missing != NULL ) {
            XFreeStringList(missing);
        }
        if ( data->font_set == NULL ) {
            SDL_SetError("Couldn't load font %s", g_MessageBoxFont);
            return -1;
        }
    } else {
        data->font_struct = XLoadQueryFont( data->display, g_MessageBoxFontLatin1 );
        if ( data->font_struct == NULL ) {
            SDL_SetError("Couldn't load font %s", g_MessageBoxFontLatin1);
            return -1;
        }
    }

    if ( messageboxdata->colorScheme ) {
        colorhints = messageboxdata->colorScheme->colors;
    } else {
        colorhints = g_default_colors;
    }

    /* Convert our SDL_MessageBoxColor r,g,b values to packed RGB format. */
    for ( i = 0; i < SDL_MESSAGEBOX_COLOR_MAX; i++ ) {
        data->color[ i ] = SDL_MAKE_RGB( colorhints[ i ].r, colorhints[ i ].g, colorhints[ i ].b );
    }

    return 0;
}

/* Calculate and initialize text and button locations. */
static int
X11_MessageBoxInitPositions( SDL_MessageBoxDataX11 *data )
{
    int i;
    int ybuttons;
    int text_width_max = 0;
    int button_text_height = 0;
    int button_width = MIN_BUTTON_WIDTH;
    const SDL_MessageBoxData *messageboxdata = data->messageboxdata;

    /* Go over text and break linefeeds into separate lines. */
    if ( messageboxdata->message && messageboxdata->message[ 0 ] ) {
        const char *text = messageboxdata->message;
        TextLineData *plinedata = data->linedata;

        for ( i = 0; i < MAX_TEXT_LINES; i++, plinedata++ ) {
            int height;
            char *lf = SDL_strchr( ( char * )text, '\n' );

            data->numlines++;

            /* Only grab length up to lf if it exists and isn't the last line. */
            plinedata->length = ( lf && ( i < MAX_TEXT_LINES - 1 ) ) ? ( lf - text ) : SDL_strlen( text );
            plinedata->text = text;

            GetTextWidthHeight( data, text, plinedata->length, &plinedata->width, &height );

            /* Text and widths are the largest we've ever seen. */
            data->text_height = IntMax( data->text_height, height );
            text_width_max = IntMax( text_width_max, plinedata->width );

            if (lf && (lf > text) && (lf[-1] == '\r')) {
                plinedata->length--;
            }

            text += plinedata->length + 1;

            /* Break if there are no more linefeeds. */
            if ( !lf )
                break;
        }

        /* Bump up the text height slightly. */
        data->text_height += 2;
    }

    /* Loop through all buttons and calculate the button widths and height. */
    for ( i = 0; i < data->numbuttons; i++ ) {
        int height;

        data->buttonpos[ i ].buttondata = &data->buttondata[ i ];
        data->buttonpos[ i ].length = SDL_strlen( data->buttondata[ i ].text );

        GetTextWidthHeight( data, data->buttondata[ i ].text, SDL_strlen( data->buttondata[ i ].text ),
                            &data->buttonpos[ i ].text_width, &height );

        button_width = IntMax( button_width, data->buttonpos[ i ].text_width );
        button_text_height = IntMax( button_text_height, height );
    }

    if ( data->numlines ) {
        /* x,y for this line of text. */
        data->xtext = data->text_height;
        data->ytext = data->text_height + data->text_height;

        /* Bump button y down to bottom of text. */
        ybuttons = 3 * data->ytext / 2 + ( data->numlines - 1 ) * data->text_height;

        /* Bump the dialog box width and height up if needed. */
        data->dialog_width = IntMax( data->dialog_width, 2 * data->xtext + text_width_max );
        data->dialog_height = IntMax( data->dialog_height, ybuttons );
    } else {
        /* Button y starts at height of button text. */
        ybuttons = button_text_height;
    }

    if ( data->numbuttons ) {
        int x, y;
        int width_of_buttons;
        int button_spacing = button_text_height;
        int button_height = 2 * button_text_height;

        /* Bump button width up a bit. */
        button_width += button_text_height;

        /* Get width of all buttons lined up. */
        width_of_buttons = data->numbuttons * button_width + ( data->numbuttons - 1 ) * button_spacing;

        /* Bump up dialog width and height if buttons are wider than text. */
        data->dialog_width = IntMax( data->dialog_width, width_of_buttons + 2 * button_spacing );
        data->dialog_height = IntMax( data->dialog_height, ybuttons + 2 * button_height );

        /* Location for first button. */
        x = ( data->dialog_width - width_of_buttons ) / 2;
        y = ybuttons + ( data->dialog_height - ybuttons - button_height ) / 2;

        for ( i = 0; i < data->numbuttons; i++ ) {
            /* Button coordinates. */
            data->buttonpos[ i ].rect.x = x;
            data->buttonpos[ i ].rect.y = y;
            data->buttonpos[ i ].rect.w = button_width;
            data->buttonpos[ i ].rect.h = button_height;

            /* Button text coordinates. */
            data->buttonpos[ i ].x = x + ( button_width - data->buttonpos[ i ].text_width ) / 2;
            data->buttonpos[ i ].y = y + ( button_height - button_text_height - 1 ) / 2 + button_text_height;

            /* Scoot over for next button. */
            x += button_width + button_spacing;
        }
    }

    return 0;
}

/* Free SDL_MessageBoxData data. */
static void
X11_MessageBoxShutdown( SDL_MessageBoxDataX11 *data )
{
    if ( data->font_set != NULL ) {
        XFreeFontSet( data->display, data->font_set );
        data->font_set = NULL;
    }

    if ( data->font_struct != NULL ) {
        XFreeFont( data->display, data->font_struct );
        data->font_struct = NULL;
    }

    if ( data->display ) {
        if ( data->window != None ) {
            XUnmapWindow( data->display, data->window );
            XDestroyWindow( data->display, data->window );
            data->window = None;
        }

        XCloseDisplay( data->display );
        data->display = NULL;
    }
}

/* Create and set up our X11 dialog box indow. */
static int
X11_MessageBoxCreateWindow( SDL_MessageBoxDataX11 *data )
{
    int x, y;
    XSizeHints *sizehints;
    XSetWindowAttributes wnd_attr;
    Display *display = data->display;
    SDL_WindowData *windowdata = NULL;
    const SDL_MessageBoxData *messageboxdata = data->messageboxdata;

    if ( messageboxdata->window ) {
        windowdata = (SDL_WindowData *)messageboxdata->window->driverdata;
    }

    data->event_mask = ExposureMask |
                       ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask |
                       StructureNotifyMask | FocusChangeMask | PointerMotionMask;
    wnd_attr.event_mask = data->event_mask;

    data->window = XCreateWindow(
                       display, DefaultRootWindow( display ),
                       0, 0,
                       data->dialog_width, data->dialog_height,
                       0, CopyFromParent, InputOutput, CopyFromParent,
                       CWEventMask, &wnd_attr );
    if ( data->window == None ) {
        SDL_SetError("Couldn't create X window");
        return -1;
    }

    if ( windowdata ) {
        /* http://tronche.com/gui/x/icccm/sec-4.html#WM_TRANSIENT_FOR */
        XSetTransientForHint( display, data->window, windowdata->xwindow );
    }

    XStoreName( display, data->window, messageboxdata->title );

    /* Allow the window to be deleted by the window manager */
    data->wm_protocols = XInternAtom( display, "WM_PROTOCOLS", False );
    data->wm_delete_message = XInternAtom( display, "WM_DELETE_WINDOW", False );
    XSetWMProtocols( display, data->window, &data->wm_delete_message, 1 );

    if ( windowdata ) {
        XWindowAttributes attrib;
        Window dummy;

        XGetWindowAttributes(display, windowdata->xwindow, &attrib);
        x = attrib.x + ( attrib.width - data->dialog_width ) / 2;
        y = attrib.y + ( attrib.height - data->dialog_height ) / 3 ;
        XTranslateCoordinates(display, windowdata->xwindow, DefaultRootWindow( display ), x, y, &x, &y, &dummy);
    } else {
        int screen = DefaultScreen( display );
        x = ( DisplayWidth( display, screen ) - data->dialog_width ) / 2;
        y = ( DisplayHeight( display, screen ) - data->dialog_height ) / 3 ;
    }
    XMoveWindow( display, data->window, x, y );

    sizehints = XAllocSizeHints();
    if ( sizehints ) {
        sizehints->flags = USPosition | USSize | PMaxSize | PMinSize;
        sizehints->x = x;
        sizehints->y = y;
        sizehints->width = data->dialog_width;
        sizehints->height = data->dialog_height;

        sizehints->min_width = sizehints->max_width = data->dialog_width;
        sizehints->min_height = sizehints->max_height = data->dialog_height;

        XSetWMNormalHints( display, data->window, sizehints );

        XFree( sizehints );
    }

    XMapRaised( display, data->window );
    return 0;
}

/* Draw our message box. */
static void
X11_MessageBoxDraw( SDL_MessageBoxDataX11 *data, GC ctx )
{
    int i;
    Window window = data->window;
    Display *display = data->display;

    XSetForeground( display, ctx, data->color[ SDL_MESSAGEBOX_COLOR_BACKGROUND ] );
    XFillRectangle( display, window, ctx, 0, 0, data->dialog_width, data->dialog_height );

    XSetForeground( display, ctx, data->color[ SDL_MESSAGEBOX_COLOR_TEXT ] );
    for ( i = 0; i < data->numlines; i++ ) {
        TextLineData *plinedata = &data->linedata[ i ];

        if (SDL_X11_HAVE_UTF8) {
            Xutf8DrawString( display, window, data->font_set, ctx,
                             data->xtext, data->ytext + i * data->text_height,
                             plinedata->text, plinedata->length );
        } else {
            XDrawString( display, window, ctx,
                         data->xtext, data->ytext + i * data->text_height,
                         plinedata->text, plinedata->length );
        }
    }

    for ( i = 0; i < data->numbuttons; i++ ) {
        SDL_MessageBoxButtonDataX11 *buttondatax11 = &data->buttonpos[ i ];
        const SDL_MessageBoxButtonData *buttondata = buttondatax11->buttondata;
        int border = ( buttondata->flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT ) ? 2 : 0;
        int offset = ( ( data->mouse_over_index == i ) && ( data->button_press_index == data->mouse_over_index ) ) ? 1 : 0;

        XSetForeground( display, ctx, data->color[ SDL_MESSAGEBOX_COLOR_BUTTON_BACKGROUND ] );
        XFillRectangle( display, window, ctx,
                        buttondatax11->rect.x - border, buttondatax11->rect.y - border,
                        buttondatax11->rect.w + 2 * border, buttondatax11->rect.h + 2 * border );

        XSetForeground( display, ctx, data->color[ SDL_MESSAGEBOX_COLOR_BUTTON_BORDER ] );
        XDrawRectangle( display, window, ctx,
                        buttondatax11->rect.x, buttondatax11->rect.y,
                        buttondatax11->rect.w, buttondatax11->rect.h );

        XSetForeground( display, ctx, ( data->mouse_over_index == i ) ?
                        data->color[ SDL_MESSAGEBOX_COLOR_BUTTON_SELECTED ] :
                        data->color[ SDL_MESSAGEBOX_COLOR_TEXT ] );

        if (SDL_X11_HAVE_UTF8) {
            Xutf8DrawString( display, window, data->font_set, ctx,
                             buttondatax11->x + offset,
                             buttondatax11->y + offset,
                             buttondata->text, buttondatax11->length );
        } else {
            XDrawString( display, window, ctx,
                         buttondatax11->x + offset, buttondatax11->y + offset,
                         buttondata->text, buttondatax11->length );
        }
    }
}

/* Loop and handle message box event messages until something kills it. */
static int
X11_MessageBoxLoop( SDL_MessageBoxDataX11 *data )
{
    GC ctx;
    XGCValues ctx_vals;
    SDL_bool close_dialog = SDL_FALSE;
    SDL_bool has_focus = SDL_TRUE;
    KeySym last_key_pressed = XK_VoidSymbol;
    unsigned long gcflags = GCForeground | GCBackground;

    SDL_zero(ctx_vals);
    ctx_vals.foreground = data->color[ SDL_MESSAGEBOX_COLOR_BACKGROUND ];
    ctx_vals.background = data->color[ SDL_MESSAGEBOX_COLOR_BACKGROUND ];

    if (!SDL_X11_HAVE_UTF8) {
        gcflags |= GCFont;
        ctx_vals.font = data->font_struct->fid;
    }

    ctx = XCreateGC( data->display, data->window, gcflags, &ctx_vals );
    if ( ctx == None ) {
        SDL_SetError("Couldn't create graphics context");
        return -1;
    }

    data->button_press_index = -1;  /* Reset what button is currently depressed. */
    data->mouse_over_index = -1;    /* Reset what button the mouse is over. */

    while( !close_dialog ) {
        XEvent e;
        SDL_bool draw = SDL_TRUE;

        XWindowEvent( data->display, data->window, data->event_mask, &e );

        /* If XFilterEvent returns True, then some input method has filtered the
           event, and the client should discard the event. */
        if ( ( e.type != Expose ) && XFilterEvent( &e, None ) )
            continue;

        switch( e.type ) {
        case Expose:
            if ( e.xexpose.count > 0 ) {
                draw = SDL_FALSE;
            }
            break;

        case FocusIn:
            /* Got focus. */
            has_focus = SDL_TRUE;
            break;

        case FocusOut:
            /* lost focus. Reset button and mouse info. */
            has_focus = SDL_FALSE;
            data->button_press_index = -1;
            data->mouse_over_index = -1;
            break;

        case MotionNotify:
            if ( has_focus ) {
                /* Mouse moved... */
                data->mouse_over_index = GetHitButtonIndex( data, e.xbutton.x, e.xbutton.y );
            }
            break;

        case ClientMessage:
            if ( e.xclient.message_type == data->wm_protocols &&
                 e.xclient.format == 32 &&
                 e.xclient.data.l[ 0 ] == data->wm_delete_message ) {
                close_dialog = SDL_TRUE;
            }
            break;

        case KeyPress:
            /* Store key press - we make sure in key release that we got both. */
            last_key_pressed = XLookupKeysym( &e.xkey, 0 );
            break;

        case KeyRelease: {
            Uint32 mask = 0;
            KeySym key = XLookupKeysym( &e.xkey, 0 );

            /* If this is a key release for something we didn't get the key down for, then bail. */
            if ( key != last_key_pressed )
                break;

            if ( key == XK_Escape )
                mask = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
            else if ( ( key == XK_Return ) || ( key == XK_KP_Enter ) )
                mask = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;

            if ( mask ) {
                int i;

                /* Look for first button with this mask set, and return it if found. */
                for ( i = 0; i < data->numbuttons; i++ ) {
                    SDL_MessageBoxButtonDataX11 *buttondatax11 = &data->buttonpos[ i ];

                    if ( buttondatax11->buttondata->flags & mask ) {
                        *data->pbuttonid = buttondatax11->buttondata->buttonid;
                        close_dialog = SDL_TRUE;
                        break;
                    }
                }
            }
            break;
        }

        case ButtonPress:
            data->button_press_index = -1;
            if ( e.xbutton.button == Button1 ) {
                /* Find index of button they clicked on. */
                data->button_press_index = GetHitButtonIndex( data, e.xbutton.x, e.xbutton.y );
            }
            break;

        case ButtonRelease:
            /* If button is released over the same button that was clicked down on, then return it. */
            if ( ( e.xbutton.button == Button1 ) && ( data->button_press_index >= 0 ) ) {
                int button = GetHitButtonIndex( data, e.xbutton.x, e.xbutton.y );

                if ( data->button_press_index == button ) {
                    SDL_MessageBoxButtonDataX11 *buttondatax11 = &data->buttonpos[ button ];

                    *data->pbuttonid = buttondatax11->buttondata->buttonid;
                    close_dialog = SDL_TRUE;
                }
            }
            data->button_press_index = -1;
            break;
        }

        if ( draw ) {
            /* Draw our dialog box. */
            X11_MessageBoxDraw( data, ctx );
        }
    }

    XFreeGC( data->display, ctx );
    return 0;
}

static int
X11_ShowMessageBoxImpl(const SDL_MessageBoxData *messageboxdata, int *buttonid)
{
    int ret;
    SDL_MessageBoxDataX11 data;
    char *origlocale;

    SDL_zero(data);

    if ( !SDL_X11_LoadSymbols() )
        return -1;

    origlocale = setlocale(LC_ALL, NULL);
    if (origlocale != NULL) {
        origlocale = SDL_strdup(origlocale);
        if (origlocale == NULL) {
            SDL_OutOfMemory();
            return -1;
        }
        setlocale(LC_ALL, "");
    }

    /* This code could get called from multiple threads maybe? */
    XInitThreads();

    /* Initialize the return buttonid value to -1 (for error or dialogbox closed). */
    *buttonid = -1;

    /* Init and display the message box. */
    ret = X11_MessageBoxInit( &data, messageboxdata, buttonid );
    if ( ret != -1 ) {
        ret = X11_MessageBoxInitPositions( &data );
        if ( ret != -1 ) {
            ret = X11_MessageBoxCreateWindow( &data );
            if ( ret != -1 ) {
                ret = X11_MessageBoxLoop( &data );
            }
        }
    }

    X11_MessageBoxShutdown( &data );

    if (origlocale) {
        setlocale(LC_ALL, origlocale);
        SDL_free(origlocale);
    }

    return ret;
}

/* Display an x11 message box. */
int
X11_ShowMessageBox(const SDL_MessageBoxData *messageboxdata, int *buttonid)
{
#if SDL_FORK_MESSAGEBOX
    /* Use a child process to protect against setlocale(). Annoying. */
    pid_t pid;
    int fds[2];
    int status = 0;

    if (pipe(fds) == -1) {
        return X11_ShowMessageBoxImpl(messageboxdata, buttonid); /* oh well. */
    }

    pid = fork();
    if (pid == -1) {  /* failed */
        close(fds[0]);
        close(fds[1]);
        return X11_ShowMessageBoxImpl(messageboxdata, buttonid); /* oh well. */
    } else if (pid == 0) {  /* we're the child */
        int exitcode = 0;
        close(fds[0]);
        status = X11_ShowMessageBoxImpl(messageboxdata, buttonid);
        if (write(fds[1], &status, sizeof (int)) != sizeof (int))
            exitcode = 1;
        else if (write(fds[1], buttonid, sizeof (int)) != sizeof (int))
            exitcode = 1;
        close(fds[1]);
        _exit(exitcode);  /* don't run atexit() stuff, static destructors, etc. */
    } else {  /* we're the parent */
        pid_t rc;
        close(fds[1]);
        do {
            rc = waitpid(pid, &status, 0);
        } while ((rc == -1) && (errno == EINTR));

        SDL_assert(rc == pid);  /* not sure what to do if this fails. */

        if ((rc == -1) || (!WIFEXITED(status)) || (WEXITSTATUS(status) != 0)) {
            SDL_SetError("msgbox child process failed");
            return -1;
        }

        if (read(fds[0], &status, sizeof (int)) != sizeof (int))
            status = -1;
        else if (read(fds[0], buttonid, sizeof (int)) != sizeof (int))
            status = -1;
        close(fds[0]);

        return status;
    }
#else
    return X11_ShowMessageBoxImpl(messageboxdata, buttonid);
#endif
}
#endif /* SDL_VIDEO_DRIVER_X11 */

/* vi: set ts=4 sw=4 expandtab: */
