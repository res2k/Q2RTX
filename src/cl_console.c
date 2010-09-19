/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// console.c

#include "cl_local.h"
#include "prompt.h"

#define CON_TIMES       16
#define CON_TIMES_MASK  ( CON_TIMES - 1 )

#define CON_TOTALLINES          1024    // total lines in console scrollback
#define CON_TOTALLINES_MASK     ( CON_TOTALLINES - 1 )

#define CON_LINEWIDTH   100     // fixed width, do not need more

typedef enum {
    CHAT_NONE,
    CHAT_DEFAULT,
    CHAT_TEAM
} chatMode_t;

typedef enum {
    CON_DEFAULT,
    CON_CHAT,
    CON_REMOTE
} consoleMode_t;

typedef struct console_s {
    qboolean    initialized;

    char    text[CON_TOTALLINES][CON_LINEWIDTH];
    int     current;        // line where next message will be printed
    int     x;              // offset in current line for next print
    int     display;        // bottom of console displays this line
    int     color;
    qboolean newline;

    int     linewidth;      // characters across screen
    int     vidWidth, vidHeight;
    float   scale;

    unsigned    times[CON_TIMES];   // cls.realtime time the line was generated
                                // for transparent notify lines
    qboolean    skipNotify;

    qhandle_t   backImage;
    qhandle_t   charsetImage;

    float   currentHeight;  // aproaches scr_conlines at scr_conspeed
    float   destHeight;     // 0.0 to 1.0 lines of console to display

    commandPrompt_t chatPrompt;
    commandPrompt_t prompt;

    chatMode_t chat;
    consoleMode_t mode;
    netadr_t remoteAddress;
    char *remotePassword;
} console_t;

static console_t    con;

static cvar_t   *con_notifytime;
static cvar_t   *con_notifylines;
static cvar_t   *con_clock;
static cvar_t   *con_height;
static cvar_t   *con_speed;
static cvar_t   *con_alpha;
static cvar_t   *con_scale;
static cvar_t   *con_font;
static cvar_t   *con_background;
static cvar_t   *con_scroll;
static cvar_t   *con_history;

// ============================================================================

/*
================
Con_SkipNotify
================
*/
void Con_SkipNotify( qboolean skip ) {
    con.skipNotify = skip;
}

/*
================
Con_ClearTyping
================
*/
void Con_ClearTyping( void ) {
    // clear any typing
    IF_Clear( &con.prompt.inputLine );
    Prompt_ClearState( &con.prompt );
}

/*
================
Con_Close
================
*/
void Con_Close( void ) {
    Con_ClearTyping();
    Con_ClearNotify_f();

    Key_SetDest( cls.key_dest & ~KEY_CONSOLE );

    con.destHeight = con.currentHeight = 0;
    con.mode = CON_DEFAULT;
    con.chat = CHAT_DEFAULT;
}

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f( void ) {
    SCR_EndLoadingPlaque();    // get rid of loading plaque

    Con_ClearTyping();
    Con_ClearNotify_f();

    if( cls.key_dest & KEY_CONSOLE ) {
        Key_SetDest( cls.key_dest & ~KEY_CONSOLE );
        return;
    }

    // FIXME: use old q2 style
    Key_SetDest( ( cls.key_dest | KEY_CONSOLE ) & ~KEY_MESSAGE );
    //con.mode = CON_DEFAULT;
}

/*
================
Con_ToggleChat_f
================
*/
static void Con_ToggleChat_f( void ) {
    Con_ToggleConsole_f();

    if( ( cls.key_dest & KEY_CONSOLE ) && cls.state == ca_active ) {
        con.mode = CON_CHAT;
        con.chat = CHAT_DEFAULT;
    }
}

/*
================
Con_ToggleChat2_f
================
*/
static void Con_ToggleChat2_f( void ) {
    Con_ToggleConsole_f();

    if( ( cls.key_dest & KEY_CONSOLE ) && cls.state == ca_active ) {
        con.mode = CON_CHAT;
        con.chat = CHAT_TEAM;
    }
}

/*
================
Con_Clear_f
================
*/
static void Con_Clear_f( void ) {
    memset( con.text, 0, sizeof( con.text ) );
    con.display = con.current;
}

static void Con_Dump_c( genctx_t *ctx, int argnum ) {
    if( argnum == 1 ) {
        FS_File_g( "condumps", ".txt", 0x80000000, ctx );
    }
}

/*
================
Con_Dump_f

Save the console contents out to a file
================
*/
static void Con_Dump_f( void ) {
    int     l;
    char    *line;
    qhandle_t f;
    char    name[MAX_OSPATH];

    if( Cmd_Argc() != 2 ) {
        Com_Printf( "Usage: %s <filename>\n", Cmd_Argv( 0 ) );
        return;
    }

    f = FS_EasyOpenFile( name, sizeof( name ), FS_MODE_WRITE,
        "condumps/", Cmd_Argv( 1 ), ".txt" );
    if( !f ) {
        return;
    }

    // skip empty lines
    for( l = con.current - CON_TOTALLINES + 1 ; l <= con.current ; l++ ) {
        if( con.text[l & CON_TOTALLINES_MASK][0] ) {
            break;
        }
    }

    // write the remaining lines
    for( ; l <= con.current ; l++ ) {
        line = con.text[l & CON_TOTALLINES_MASK];
        FS_FPrintf( f, "%s\n", line + 1 );
    }

    FS_FCloseFile( f );

    Com_Printf( "Dumped console text to %s.\n", name );
}

                        
/*
================
Con_ClearNotify_f
================
*/
void Con_ClearNotify_f( void ) {
    int     i;
    
    for( i = 0; i < CON_TIMES; i++ )
        con.times[i] = 0;
}

                        
/*
================
Con_MessageMode_f
================
*/
static void Con_MessageMode_f( void ) {
    Con_Close();

    con.chat = CHAT_DEFAULT;
    IF_Replace( &con.chatPrompt.inputLine, Cmd_RawArgs() );
    Key_SetDest( cls.key_dest | KEY_MESSAGE );
}

/*
================
Con_MessageMode2_f
================
*/
static void Con_MessageMode2_f( void ) {
    Con_Close();

    con.chat = CHAT_TEAM;
    IF_Replace( &con.chatPrompt.inputLine, Cmd_RawArgs() );
    Key_SetDest( cls.key_dest | KEY_MESSAGE );
}

static void Con_RemoteMode_f( void ) {
    netadr_t adr;
    char *s;

    if( Cmd_Argc() != 3 ) {
        Com_Printf( "Usage: %s <address> <password>\n", Cmd_Argv( 0 ) );
        return;
    }

    s = Cmd_Argv( 1 );
    if( !NET_StringToAdr( s, &adr, PORT_SERVER ) ) {
        Com_Printf( "Bad address: %s\n", s );
        return;
    }

    s = Cmd_Argv( 2 );

    if( !( cls.key_dest & KEY_CONSOLE ) ) {
        Con_ToggleConsole_f();
    }

    con.mode = CON_REMOTE;
    con.remoteAddress = adr;
    if( con.remotePassword ) {
        Z_Free( con.remotePassword );
    }
    con.remotePassword = Z_CopyString( s );
}

static void CL_RemoteMode_c( genctx_t *ctx, int argnum ) {
    if( argnum == 1 ) {
        Com_Address_g( ctx );
    }
}

/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
static void Con_CheckResize( void ) {
    int     width;

    con.vidWidth = scr_glconfig.vidWidth * con.scale;
    con.vidHeight = scr_glconfig.vidHeight * con.scale;

    width = ( con.vidWidth / CHAR_WIDTH ) - 2;

    if( width == con.linewidth )
        return;

    con.linewidth = width > CON_LINEWIDTH ? CON_LINEWIDTH : width;
    con.prompt.inputLine.visibleChars = con.linewidth;
    con.prompt.widthInChars = con.linewidth - 1; // account for color byte
    con.chatPrompt.inputLine.visibleChars = con.linewidth;
}

/*
================
Con_CheckTop

Make sure at least one line is visible if console is backscrolled.
================
*/
static void Con_CheckTop( void ) {
    int top = con.current - CON_TOTALLINES + 1;

    if( top < 1 ) {
        top = 1;
    }
    if( con.display < top ) {
        con.display = top;
    }
}

static void con_param_changed( cvar_t *self ) {
    if( con.initialized && cls.ref_initialized ) {
        Con_RegisterMedia();
    }
}

static const cmdreg_t c_console[] = {
    { "toggleconsole", Con_ToggleConsole_f },
    { "togglechat", Con_ToggleChat_f },
    { "togglechat2", Con_ToggleChat2_f },
    { "messagemode", Con_MessageMode_f },
    { "messagemode2", Con_MessageMode2_f },
    { "remotemode", Con_RemoteMode_f, CL_RemoteMode_c },
    { "clear", Con_Clear_f },
    { "clearnotify", Con_ClearNotify_f },
    { "condump", Con_Dump_f, Con_Dump_c },

    { NULL }
};

/*
================
Con_Init
================
*/
void Con_Init( void ) {
    memset( &con, 0, sizeof( con ) );

//
// register our commands
//
    Cmd_Register( c_console );

    con_notifytime = Cvar_Get( "con_notifytime", "3", 0 );
    con_notifylines = Cvar_Get( "con_notifylines", "4", 0 );
    con_clock = Cvar_Get( "con_clock", "0", CVAR_ARCHIVE );
    con_height = Cvar_Get( "con_height", "0.5", CVAR_ARCHIVE );
    con_speed = Cvar_Get( "scr_conspeed", "3", 0 );
    con_alpha = Cvar_Get( "con_alpha", "1", CVAR_ARCHIVE );
    con_scale = Cvar_Get( "con_scale", "1", CVAR_ARCHIVE );
    con_font = Cvar_Get( "con_font", "conchars", CVAR_ARCHIVE );
    con_font->changed = con_param_changed;
    con_background = Cvar_Get( "con_background", "conback", CVAR_ARCHIVE );
    con_background->changed = con_param_changed;
    con_scroll = Cvar_Get( "con_scroll", "0", CVAR_ARCHIVE );
    con_history = Cvar_Get( "con_history", "0", 0 );

    IF_Init( &con.prompt.inputLine, 0, MAX_FIELD_TEXT - 1 );
    IF_Init( &con.chatPrompt.inputLine, 0, MAX_FIELD_TEXT - 1 );

    con.prompt.printf = Con_Printf;

    // use default width if no video initialized yet
    scr_glconfig.vidWidth = 640;
    scr_glconfig.vidHeight = 480;
    con.linewidth = -1;
    con.scale = 1;
    con.color = COLOR_NONE;
    con.text[0][0] = COLOR_NONE;
    con.x = 1;

    Con_CheckResize();

    con.initialized = qtrue;
}

void Con_PostInit( void ) {
    if( con_history->integer > 0 ) {
        Prompt_LoadHistory( &con.prompt, COM_HISTORYFILE_NAME );
    }
}

/*
================
Con_Shutdown
================
*/
void Con_Shutdown( void ) {
    if( con_history->integer > 0 ) {
        Prompt_SaveHistory( &con.prompt, COM_HISTORYFILE_NAME, con_history->integer );
    }
    Prompt_Clear( &con.prompt );
}

static void Con_CarriageRet( void ) {
    char *p;

    p = con.text[con.current & CON_TOTALLINES_MASK];
    memset( p, 0, sizeof( con.text[0] ) );

    // add color from last line
    con.x = 0;
    p[con.x++] = con.color;
}

/*
===============
Con_Linefeed
===============
*/
static void Con_Linefeed( void ) {
    if( con.display == con.current )
        con.display++;
    con.current++;

    Con_CarriageRet();

    if( con_scroll->integer & 2 ) {
        con.display = con.current;
    } else {
        Con_CheckTop();
    }
}

void Con_SetColor( color_index_t color ) {
    con.color = color;
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be displayed on screen
If no console is visible, the text will appear at the top of the game window
================
*/
void Con_Print( const char *txt ) {
    int prevline;
    char *p;
    int l;

    if( !con.initialized )
        return;

    prevline = con.current;

    while( *txt ) {
        if( con.newline ) {
            Con_Linefeed();
            con.newline = qfalse;
        }

    // count word length
        for( p = ( char * )txt; *p > 32; p++ )
            ;
        l = p - txt;

    // word wrap
        if( l < con.linewidth && con.x + l > con.linewidth ) {
            Con_Linefeed();
        }

        switch( *txt ) {
        case '\r':
            Con_CarriageRet();
            break;
        case '\n':
            con.newline = qtrue;
            break;
        default:    // display character and advance
            if( con.x == con.linewidth ) {
                Con_Linefeed();
            }
            p = con.text[con.current & CON_TOTALLINES_MASK];
            p[con.x++] = *txt;
            break;
        }

        txt++;
    }

    // update time for transparent overlay
    if( !con.skipNotify ) {
        for( l = prevline + 1; l <= con.current; l++ ) {
            con.times[l & CON_TIMES_MASK] = cls.realtime;
        }
    }
}

/*
================
Con_Printf

Print text to graphical console only,
bypassing system console and logfiles
================
*/
void Con_Printf( const char *fmt, ... ) {
    va_list     argptr;
    char        msg[MAXPRINTMSG];

    va_start( argptr, fmt );
    Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
    va_end( argptr );

    Con_Print( msg );
}

/*
================
Con_RegisterMedia
================
*/
void Con_RegisterMedia( void ) {
    qerror_t ret;

    ret = _R_RegisterFont( con_font->string, &con.charsetImage );
    if( !con.charsetImage ) {
        if( strcmp( con_font->string, "conchars" ) ) {
            Com_WPrintf( "Couldn't load console font: %s\n", Q_ErrorString( ret ) );
            Cvar_Reset( con_font );
            ret = _R_RegisterFont( "conchars", &con.charsetImage );
        }
        if( !con.charsetImage ) {
            Com_Error( ERR_FATAL, "Couldn't load pics/conchars.pcx: %s", Q_ErrorString( ret ) );
        }
    }

    ret = _R_RegisterPic( con_background->string, &con.backImage );
    if( !con.backImage ) {
        if( strcmp( con_background->string, "conback" ) ) {
            Com_WPrintf( "Couldn't load console background: %s\n", Q_ErrorString( ret ) );
            Cvar_Reset( con_background );
            ret = _R_RegisterPic( "conback", &con.backImage );
        }
        if( !con.charsetImage ) {
            Com_EPrintf( "Couldn't load pics/conback.pcx: %s\n", Q_ErrorString( ret ) );
        }
    }
}

/*
==============================================================================

DRAWING

==============================================================================
*/

static int Con_DrawLine( int v, int line, float alpha ) {
    char *p = con.text[line & CON_TOTALLINES_MASK];
    color_index_t c = *p;
    color_t color;
    int flags = 0;

    switch( c ) {
    case COLOR_ALT:
        flags = UI_ALTCOLOR;
        // fall through
    case COLOR_NONE:
        R_SetColor( DRAW_COLOR_CLEAR, NULL );
        break;
    default:
        VectorCopy( colorTable[c & 7], color );
        color[3] = alpha * 255;
        R_SetColor( DRAW_COLOR_RGBA, color );
        break;
    }

    return R_DrawString( CHAR_WIDTH, v, flags, con.linewidth - 1, p + 1,
        con.charsetImage );
}

#define CON_PRESTEP     ( 10 + CHAR_HEIGHT * 2 )

/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify( void ) {
    int     v;
    char    *text;
    int     i, j;
    unsigned    time;
    int     skip;
    float   alpha;

    // only draw notify in game
    if( cls.state != ca_active ) {
        return; 
    }
    if( cls.key_dest & ( KEY_MENU|KEY_CONSOLE ) ) {
        return;
    }
    if( con.currentHeight ) {
        return;
    }

    j = con_notifylines->integer;
    if( j > CON_TIMES ) {
        j = CON_TIMES;
    }

    v = 0;
    for( i = con.current - j + 1; i <= con.current; i++ ) {
        if( i < 0 )
            continue;
        time = con.times[i & CON_TIMES_MASK];
        if( time == 0 )
            continue;
        // alpha fade the last string left on screen
        alpha = SCR_FadeAlpha( time, con_notifytime->value * 1000, 300 );
        if( !alpha )
            continue;
        if( v || i != con.current ) {
            alpha = 1;  // don't fade
        }

        Con_DrawLine( v, i, alpha );

        v += CHAR_HEIGHT;
    }
    
    R_SetColor( DRAW_COLOR_CLEAR, NULL );

    if( cls.key_dest & KEY_MESSAGE ) {
        if( con.chat == CHAT_TEAM ) {
            text = "say_team:";
            skip = 11;
        } else {
            text = "say:";
            skip = 5;
        }

        R_DrawString( CHAR_WIDTH, v, 0, MAX_STRING_CHARS, text,
            con.charsetImage );
        con.chatPrompt.inputLine.visibleChars = con.linewidth - skip + 1;
        IF_Draw( &con.chatPrompt.inputLine, skip * CHAR_WIDTH, v,
            UI_DRAWCURSOR, con.charsetImage );
    }
}

/*
================
Con_DrawSolidConsole

Draws the console with the solid background
================
*/
void Con_DrawSolidConsole( void ) {
    int             i, x, y;
    int             rows;
    char            *text;
    int             row;
    char            buffer[CON_LINEWIDTH];
    int             vislines;
    float           alpha;
    clipRect_t      clip;
    int             widths[2];

    vislines = con.vidHeight * con.currentHeight;
    if( vislines <= 0 )
        return;

    if( vislines > con.vidHeight )
        vislines = con.vidHeight;

// setup transparency
    if( cls.state == ca_active &&
        con_alpha->value &&
        ( cls.key_dest & KEY_MENU ) == 0 )
    {
        alpha = 0.5f + 0.5f * ( con.currentHeight / con_height->value );

        Cvar_ClampValue( con_alpha, 0, 1 );
        alpha *= con_alpha->value;

        R_SetColor( DRAW_COLOR_ALPHA, ( byte * )&alpha );
    }

    clip.left = 0;
    clip.top = 0;
    clip.right = 0;
    clip.bottom = 0;
    R_SetClipRect( DRAW_CLIP_TOP, &clip );

// draw the background
    if( cls.state != ca_active || ( cls.key_dest & KEY_MENU ) || con_alpha->value ) {
        R_DrawStretchPic( 0, vislines - con.vidHeight,
            con.vidWidth, con.vidHeight, con.backImage );
    }
#if 0
    if( cls.state > ca_disconnected && cls.state < ca_active ) {
        R_DrawFill( 0, vislines, con.vidWidth, con.vidHeight - vislines, 0 );
    }
#endif

// draw the text
    y = vislines - CON_PRESTEP;
    rows = y / CHAR_HEIGHT + 1;     // rows of text to draw

// draw arrows to show the buffer is backscrolled
    if( con.display != con.current ) {
        R_SetColor( DRAW_COLOR_RGBA, colorRed );
        for( i = 1; i < con.linewidth / 2; i += 4 ) {
            R_DrawChar( i * CHAR_WIDTH, y, 0, '^', con.charsetImage );
        }
    
        y -= CHAR_HEIGHT;
        rows--;
    }
    
// draw from the bottom up
    R_SetColor( DRAW_COLOR_CLEAR, NULL );
    row = con.display;
    for( i = 0; i < rows; i++ ) {
        if( row < 0 )
            break;
        if( con.current - row > CON_TOTALLINES - 1 )
            break;      // past scrollback wrap point
            
        x = Con_DrawLine( y, row, 1 );
        if( i < 2 ) {
            widths[i] = x;
        }

        y -= CHAR_HEIGHT;
        row--;
    }
    
    R_SetColor( DRAW_COLOR_CLEAR, NULL );

//ZOID
    // draw the download bar
    // figure out width
    if( cls.download.name[0] ) {
        int n, j;

        if( ( text = strrchr( cls.download.name, '/') ) != NULL )
            text++;
        else
            text = cls.download.name;

        x = con.linewidth - ( ( con.linewidth * 7 ) / 40 );
        y = x - strlen( text ) - 8;
        i = con.linewidth / 3;
        if ( strlen( text ) > i ) {
            y = x - i - 11;
            strncpy( buffer, text, i );
            buffer[i] = 0;
            strcat( buffer, "..." );
        } else {
            strcpy( buffer, text );
        }
        strcat( buffer, ": " );
        i = strlen( buffer );
        buffer[i++] = '\x80';
        // where's the dot go?
        n = y * cls.download.percent / 100;     
        for ( j = 0; j < y; j++ ) {
            if ( j == n ) {
                buffer[i++] = '\x83';
            } else {
                buffer[i++] = '\x81';
            }
        }
        buffer[i++] = '\x82';
        buffer[i] = 0;

        sprintf( buffer + i, " %02d%%", cls.download.percent );

        // draw it
        y = vislines - 10;
        R_DrawString( CHAR_WIDTH, y, 0, CON_LINEWIDTH, buffer, con.charsetImage );
    }
//ZOID

// draw the input prompt, user text, and cursor if desired
    x = 0;
    if( cls.key_dest & KEY_CONSOLE ) {
        y = vislines - CON_PRESTEP + CHAR_HEIGHT;

        // draw command prompt
        switch( con.mode ) {
        case CON_CHAT:
            i = '&';
            break;
        case CON_REMOTE:
            i = '#';
            break;
        default:
            i = 17;
            break;
        }
        R_SetColor( DRAW_COLOR_RGBA, colorYellow );
        R_DrawChar( CHAR_WIDTH, y, 0, i, con.charsetImage );
        R_SetColor( DRAW_COLOR_CLEAR, NULL );

        // draw input line
        x = IF_Draw( &con.prompt.inputLine, 2 * CHAR_WIDTH, y,
            UI_DRAWCURSOR, con.charsetImage );
    }

    y = vislines - CON_PRESTEP + CHAR_HEIGHT;
    row = 0;
    if( x > con.vidWidth - 12 * CHAR_WIDTH ) {
        y -= CHAR_HEIGHT;
        row++;
    }

    R_SetColor( DRAW_COLOR_RGBA, colorCyan );

// draw clock
    if( con_clock->integer ) {
        x = Com_Time_m( buffer, sizeof( buffer ) ) * CHAR_WIDTH;
        if( widths[row] + x + CHAR_WIDTH <= con.vidWidth ) {
            R_DrawString( con.vidWidth - CHAR_WIDTH - x, y - CHAR_HEIGHT,
                UI_RIGHT, MAX_STRING_CHARS, buffer, con.charsetImage );
        }
    }

// draw version
    SCR_DrawStringEx( con.vidWidth - CHAR_WIDTH, y, UI_RIGHT,
        MAX_STRING_CHARS, APPLICATION " " VERSION, con.charsetImage );

    // restore rendering parameters
    R_SetColor( DRAW_COLOR_CLEAR, NULL );
    R_SetClipRect( DRAW_CLIP_DISABLED, NULL );
}

//=============================================================================

/*
==================
Con_RunConsole

Scroll it up or down
==================
*/
void Con_RunConsole( void ) {
    if( cls.disable_screen ) {
        con.destHeight = con.currentHeight = 0;
        return;
    }

    if( !( cls.key_dest & KEY_MENU ) ) {
        if( cls.state == ca_disconnected ) {
            // draw fullscreen console
            con.destHeight = con.currentHeight = 1;
            return;
        }
        if( cls.state > ca_disconnected && cls.state < ca_active ) {
            // draw half-screen console
            con.destHeight = con.currentHeight = 0.5f;
            return;
        }
    }

// decide on the height of the console
    if( cls.key_dest & KEY_CONSOLE ) {
        con.destHeight = Cvar_ClampValue( con_height, 0.1f, 1 );
    } else {
        con.destHeight = 0;             // none visible
    }

    if( con.currentHeight > con.destHeight ) {
        con.currentHeight -= con_speed->value * cls.frametime;
        if( con.currentHeight < con.destHeight ) {
            con.currentHeight = con.destHeight;
        }
    } else if( con.currentHeight < con.destHeight ) {
        con.currentHeight += con_speed->value * cls.frametime;
        if( con.currentHeight > con.destHeight ) {
            con.currentHeight = con.destHeight;
        }
    }
}

/*
==================
SCR_DrawConsole
==================
*/
void Con_DrawConsole( void ) {
    Cvar_ClampValue( con_scale, 1, 9 );

    con.scale = 1.0f / con_scale->value;
    R_SetScale( &con.scale );

    Con_CheckResize();
    Con_DrawSolidConsole();
    Con_DrawNotify();   

    R_SetScale( NULL );
}


/*
==============================================================================

            LINE TYPING INTO THE CONSOLE AND COMMAND COMPLETION

==============================================================================
*/

static void Con_Say( char *msg ) {
    CL_ClientCommand( va( "say%s \"%s\"", con.chat == CHAT_TEAM ? "_team" : "", msg ) );
}

static void Con_Action( void ) {
    char *cmd = Prompt_Action( &con.prompt );
    
    if( !cmd ) {
        Con_Printf( "]\n" );
        return;
    }
    
    // backslash text are commands, else chat
    if( cmd[0] == '\\' || cmd[0] == '/' ) {
        if( con.mode == CON_REMOTE ) {
            CL_SendRcon( &con.remoteAddress, con.remotePassword, cmd + 1 );
        } else {
            Cbuf_AddText( &cmd_buffer, cmd + 1 );    // skip slash
            Cbuf_AddText( &cmd_buffer, "\n" );
        }
    } else {
        if( con.mode == CON_REMOTE ) {
            CL_SendRcon( &con.remoteAddress, con.remotePassword, cmd );
        } else if( cls.state == ca_active && con.mode == CON_CHAT ) {
            Con_Say( cmd );
        } else {
            Cbuf_AddText( &cmd_buffer, cmd );
            Cbuf_AddText( &cmd_buffer, "\n" );
        }
    }

    Con_Printf( "]%s\n", cmd );

    if( cls.state == ca_disconnected ) {
        SCR_UpdateScreen ();    // force an update, because the command
                                // may take some time
    }
}

/*
====================
Key_Console

Interactive line editing and console scrollback
====================
*/
void Key_Console( int key ) {
    if( key == 'l' && Key_IsDown( K_CTRL ) ) {
        Con_Clear_f();
        return;
    }

    if( key == 'd' && Key_IsDown( K_CTRL ) ) {
        con.mode = CON_DEFAULT;
        return;
    }

    if( key == K_ENTER || key == K_KP_ENTER ) {
        Con_Action();
        goto scroll;
    }

    if( ( key == 'v' && Key_IsDown( K_CTRL ) ) ||
        ( key == K_INS && Key_IsDown( K_SHIFT ) ) || key == K_MOUSE3 )
    {
        char *cbd, *s;
        
        if( ( cbd = VID_GetClipboardData() ) != NULL ) {
            s = cbd;
            while( *s ) {
                int c = *s++;
                switch( c ) {
                case '\n':
                    if( *s ) {
                        Con_Action();
                    }
                    break;
                case '\r':
                case '\t':
                    IF_CharEvent( &con.prompt.inputLine, ' ' );
                    break;
                default:
                    if( c >= 32 && c < 127 ) {
                        IF_CharEvent( &con.prompt.inputLine, c );
                    }
                    break;
                }
            }
            Z_Free( cbd );
        }
        goto scroll;
    }

    if( key == K_TAB ) {
        Prompt_CompleteCommand( &con.prompt, qtrue );
        goto scroll;
    }

    if( key == 'r' && Key_IsDown( K_CTRL ) ) {
        Prompt_CompleteHistory( &con.prompt, qfalse );
        goto scroll;
    }

    if( key == 's' && Key_IsDown( K_CTRL ) ) {
        Prompt_CompleteHistory( &con.prompt, qtrue );
        goto scroll;
    }

    if( key == K_UPARROW || ( key == 'p' && Key_IsDown( K_CTRL ) ) ) {
        Prompt_HistoryUp( &con.prompt );
        goto scroll;
    }

    if( key == K_DOWNARROW || ( key == 'n' && Key_IsDown( K_CTRL ) ) ) {
        Prompt_HistoryDown( &con.prompt );
        goto scroll;
    }

    if( key == K_PGUP || key == K_MWHEELUP ) {
        if( Key_IsDown( K_CTRL ) ) {
            con.display -= 6;
        } else {
            con.display -= 2;
        }
        Con_CheckTop();
        return;
    }

    if( key == K_PGDN || key == K_MWHEELDOWN ) {
        if( Key_IsDown( K_CTRL ) ) {
            con.display += 6;
        } else {
            con.display += 2;
        }
        if( con.display > con.current ) {
            con.display = con.current;
        }
        return;
    }

    if( key == K_HOME && Key_IsDown( K_CTRL ) ) {
        con.display = 1;
        Con_CheckTop();
        return;
    }

    if( key == K_END && Key_IsDown( K_CTRL ) ) {
        con.display = con.current;
        return;
    }

    if( IF_KeyEvent( &con.prompt.inputLine, key ) ) {
        Prompt_ClearState( &con.prompt );
    }

scroll: 
    if( con_scroll->integer & 1 ) {
        con.display = con.current;
    }
}

void Char_Console( int key ) {
    IF_CharEvent( &con.prompt.inputLine, key );
}

/*
====================
Key_Message
====================
*/
void Key_Message( int key ) {
    if( key == 'l' && Key_IsDown( K_CTRL ) ) {
        IF_Clear( &con.chatPrompt.inputLine );
        return;
    }

    if( key == K_ENTER || key == K_KP_ENTER ) {
        char *cmd = Prompt_Action( &con.chatPrompt );
        
        if( cmd ) {
            Con_Say( cmd );
        }
        Key_SetDest( cls.key_dest & ~KEY_MESSAGE );
        return;
    }

    if( key == K_ESCAPE ) {
        Key_SetDest( cls.key_dest & ~KEY_MESSAGE );
        IF_Clear( &con.chatPrompt.inputLine );
        return;
    }

    if( key == 'r' && Key_IsDown( K_CTRL ) ) {
        Prompt_CompleteHistory( &con.chatPrompt, qfalse );
        return;
    }

    if( key == 's' && Key_IsDown( K_CTRL ) ) {
        Prompt_CompleteHistory( &con.chatPrompt, qtrue );
        return;
    }

    if( key == K_UPARROW || ( key == 'p' && Key_IsDown( K_CTRL ) ) ) {
        Prompt_HistoryUp( &con.chatPrompt );
        return;
    }

    if( key == K_DOWNARROW || ( key == 'n' && Key_IsDown( K_CTRL ) ) ) {
        Prompt_HistoryDown( &con.chatPrompt );
        return;
    }

    if( IF_KeyEvent( &con.chatPrompt.inputLine, key ) ) {
        Prompt_ClearState( &con.chatPrompt );
    }
}

void Char_Message( int key ) {
    IF_CharEvent( &con.chatPrompt.inputLine, key );
}


