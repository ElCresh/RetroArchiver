#!/bin/sh
# The window shows no text of the core as it is, outside the log: an error
# or a reason goes through core_w (lang_core_text), the archive's texts
# through arc_text.c. Fails on the patterns that bypass them.
cd "$(dirname "$0")/.." || exit 2
files="src/gui/main.c src/gui/box_edit_dlg.c src/gui/settings_dlg.c src/gui/about.c"
bad=$(grep -nE 'to_w\((err\.s|r->err|r->why|why\.s|[a-z]+->error|adv)\b|wcsncpy\(g_last_err|wcsncpy\(g_why|to_w\((e->text|r->last|r->msg\.s)\b|%hs|to_w\(s, g_why|[^_]arc_(advice|outcome|media|missing)\(' $files)
if [ -n "$bad" ]; then
    echo "core_texts: a text of the core reaches the window as it is (use core_w or arc_text.c):"
    echo "$bad"
    exit 1
fi
# every message of messages.h has its key in messages_list.h
for m in $(grep -o '^#define M_[A-Z0-9_]*' src/core/messages.h | cut -d' ' -f2); do
    grep -q "MSG($m," src/core/messages_list.h || { echo "core_texts: $m has no line in messages_list.h"; exit 1; }
done
echo "core_texts: all green."
