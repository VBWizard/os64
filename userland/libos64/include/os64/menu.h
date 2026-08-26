// os64/menu.h — the menu grammar, read once and shared (2026-08-25).
//
// ONE GRAMMAR FOR EVERY LAUNCHER. The root menu (/bin/grootmenu) reads it
// today; a dock or a panel reads the same file tomorrow — Chris's ruling
// the day the launcher was designed: "dock tomorrow, both the day after"
// is the TEST of the desktop's shape, and two launchers that drift apart
// on what a menu file looks like fail it. So the parser lives here, in the
// library, and a launcher is a program that draws a tree it did not parse.
//
// The lineage is twm's .twmrc (1987): `menu "defops" { "Xterm" f.exec
// "xterm &" ... "Programs" f.menu "programs" }` — named menus, items with
// commands, cascades by reference. os64 keeps the ideas and drops the
// f.verbs; a config file should read like what it does.
//
//   # menu.conf — '#' comments, blank lines ignored
//   menu root {                        # a NAMED menu (top level only)
//       item "Terminal"  /bin/gterm    # label, then the command line
//       item "Clock"     /bin/gclock
//       separator                      # a rule
//       menu "Demos" {                 # a CASCADE, written inline
//           item "Bounce" /bin/gbounce
//       }
//       menu "Tools" tools             # a cascade BY REFERENCE (twm's f.menu)
//   }
//   menu tools {
//       item "Editor" /bin/scribe /home/notes.txt
//   }
//
// KEYWORDS FOLD CASE (they are setting names); LABELS, NAMES AND COMMANDS
// ARE DATA and are taken verbatim — the conf_path.md rule. A label is one
// token: quote it if it has spaces. A command is everything after the label
// to the end of the line; at launch it is split on whitespace, with quotes
// honoured, so `item "Notes" /bin/scribe "/home/my notes.txt"` works.
// Which named menu a launcher shows is the launcher's business (grootmenu
// shows `root` unless told otherwise); a name nobody references is not an
// error — it is a menu waiting for a launcher.
//
// Found through the config ladder like every os64 config: /home/menu.conf
// wins over /etc/menu.conf, first hit, the whole file — this is a settings
// file, not a database, so it does not merge the way `hosts` does.

#ifndef OS64_MENU_H
#define OS64_MENU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/conf.h"

#define OS64_MENU_LABEL_MAX   48    // bytes, NUL included — a menu is not a paragraph
#define OS64_MENU_COMMAND_MAX 160   // a path and a few arguments
#define OS64_MENU_NAME_MAX    32    // a named menu's name
#define OS64_MENU_NODES_MAX   256   // every item, rule and cascade in the file
#define OS64_MENU_NAMED_MAX   32    // named menus per file
#define OS64_MENU_FILE_MAX    32768 // the slurp cap; larger is refused whole

typedef enum os64_menu_kind
{
    OS64_MENU_ITEM = 1,    // label + command
    OS64_MENU_SEPARATOR,   // a rule; no label, no command
    OS64_MENU_SUBMENU,     // label + children (inline or by reference)
} os64_menu_kind_t;

typedef struct os64_menu_node
{
    uint8_t kind;                            // os64_menu_kind_t
    char    label[OS64_MENU_LABEL_MAX];
    char    command[OS64_MENU_COMMAND_MAX];  // ITEM only
    int16_t first_child;                     // SUBMENU: first child node, -1 = empty
    int16_t next;                            // next sibling, -1 = last
    char    ref[OS64_MENU_NAME_MAX];         // SUBMENU by reference: the name (resolved at load)
    uint16_t line;                           // source line, for diagnostics: a cascade's reference is
                                             // resolved in a second pass, after the parser's own
                                             // line counter is gone
} os64_menu_node_t;

typedef struct os64_menu_named
{
    char    name[OS64_MENU_NAME_MAX];
    int16_t first_child;                     // the named menu's first child, -1 = empty
} os64_menu_named_t;

typedef struct os64_menu
{
    os64_menu_node_t  nodes[OS64_MENU_NODES_MAX];
    uint16_t          count;
    os64_menu_named_t named[OS64_MENU_NAMED_MAX];
    uint16_t          named_count;
    char              path[OS64_CONF_PATH_MAX];   // the file that was read
} os64_menu_t;

typedef enum os64_menu_status
{
    OS64_MENU_OK = 0,
    OS64_MENU_NO_FILE,      // no menu.conf anywhere on the ladder (or it would not open)
    OS64_MENU_TOO_BIG,      // over OS64_MENU_FILE_MAX — refused whole, not truncated
    OS64_MENU_IO_ERROR,     // it was there and could not be read
    OS64_MENU_NO_MEMORY,
    OS64_MENU_SYNTAX,       // `err` names the line and the complaint
    OS64_MENU_TOO_MANY,     // more nodes or named menus than the arrays hold
    OS64_MENU_UNRESOLVED,   // a cascade references a name no menu defines
} os64_menu_status_t;

// Read `name` (ordinarily "menu.conf") through the config ladder and fill
// `menu`. On anything but OK the menu is unusable and `err` (may be NULL)
// holds one line saying why — with the file and line number for SYNTAX,
// TOO_MANY and UNRESOLVED, because a config the user wrote that silently
// does nothing is the afternoon a config file exists to prevent.
os64_menu_status_t os64_menu_load(os64_menu_t *menu, const char *name,
                                  char *err, size_t err_cap);

// The status as a word, for messages. Never NULL.
const char *os64_menu_status_name(os64_menu_status_t status);

// A named menu's first child, or -1 if no menu of that name was defined.
// Names are DATA: compared verbatim.
int16_t os64_menu_find(const os64_menu_t *menu, const char *name);

// Was a menu of that name defined at all? Distinct from find(): a defined
// menu with no entries answers -1 there and true here.
bool os64_menu_named_exists(const os64_menu_t *menu, const char *name);

// Split an item's command line into an argv for os64_spawn: whitespace
// separates, double quotes group, `buf` receives the NUL-separated words
// and `argv` the pointers (argv[n] = NULL). Returns the word count, 0 for
// an empty command, or negative when the words do not fit in `buf` or
// `argv` — a REFUSAL, not a truncation, because half a command line is a
// different command and the caller cannot see the difference in the argv it
// gets back.
int64_t os64_menu_argv(const char *command, char *buf, size_t buf_cap,
                       char *argv[], size_t argv_max);

#endif // OS64_MENU_H
