# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""LogView maintains a log pane's scrolling and searching state."""

from __future__ import annotations
import asyncio
import collections
import logging
import re
import time
from typing import List, Optional, TYPE_CHECKING

from prompt_toolkit.formatted_text.utils import fragment_list_to_text
from prompt_toolkit.layout.utils import explode_text_fragments
from prompt_toolkit.data_structures import Point
from prompt_toolkit.formatted_text import (
    to_formatted_text,
    fragment_list_width,
    StyleAndTextTuples,
)

import pw_console.text_formatting
from pw_console.log_store import LogStore

if TYPE_CHECKING:
    from pw_console.log_pane import LogPane

_LOG = logging.getLogger(__package__)

_UPPERCASE_REGEX = re.compile(r'[A-Z]')


class LogView:
    """Viewing window into a LogStore."""

    # pylint: disable=too-many-instance-attributes,too-many-public-methods

    def __init__(self,
                 log_pane: 'LogPane',
                 log_store: Optional[LogStore] = None):
        # Parent LogPane reference. Updated by calling `set_log_pane()`.
        self.log_pane = log_pane
        self.log_store = log_store if log_store else LogStore()
        self.log_store.register_viewer(self)

        # Search variables
        self.search_text = None
        self.search_re_flags = None
        self.search_regex = None
        self.search_highlight = False

        # Filter
        self.filtering_on = False
        self.filter_text = None
        self.filter_regex = None
        self.filtered_logs: collections.deque = collections.deque()
        self.filter_existing_logs_task = None

        # Current log line index state variables:
        self.line_index = 0
        self._last_start_index = 0
        self._last_end_index = 0
        self._current_start_index = 0
        self._current_end_index = 0

        # LogPane prompt_toolkit container render size.
        self._window_height = 20
        self._window_width = 80

        # Max frequency in seconds of prompt_toolkit UI redraws triggered by new
        # log lines.
        self._ui_update_frequency = 0.1
        self._last_ui_update_time = time.time()

        # Should new log lines be tailed?
        self.follow = True

        # Cache of formatted text tuples used in the last UI render.  Used after
        # rendering by `get_cursor_position()`.
        self._line_fragment_cache: collections.deque = collections.deque()

    def _set_match_position(self, position: int):
        self.follow = False
        self.line_index = position
        self.log_pane.application.redraw_ui()

    def search_forwards(self):
        if not self.search_regex:
            return
        self.search_highlight = True

        starting_index = self.get_current_line() + 1
        if starting_index > self.log_store.get_last_log_line_index():
            starting_index = 0

        # From current position +1 and down
        for i in range(starting_index,
                       self.log_store.get_last_log_line_index() + 1):
            if self.search_regex.search(
                    self.log_store.logs[i].ansi_stripped_log):
                self._set_match_position(i)
                return

        # From the beginning to the original start
        for i in range(0, starting_index):
            if self.search_regex.search(
                    self.log_store.logs[i].ansi_stripped_log):
                self._set_match_position(i)
                return

    def search_backwards(self):
        if not self.search_regex:
            return
        self.search_highlight = True

        starting_index = self.get_current_line() - 1
        if starting_index < 0:
            starting_index = self.log_store.get_last_log_line_index()

        # From current position - 1 and up
        for i in range(starting_index, -1, -1):
            if self.search_regex.search(
                    self.log_store.logs[i].ansi_stripped_log):
                self._set_match_position(i)
                return

        # From the end to the original start
        for i in range(self.log_store.get_last_log_line_index(),
                       starting_index, -1):
            if self.search_regex.search(
                    self.log_store.logs[i].ansi_stripped_log):
                self._set_match_position(i)
                return

    def _set_search_regex(self, text):
        # Reset search text
        self.search_text = text
        self.search_highlight = True

        # Ignorecase unless the text has capital letters in it.
        if _UPPERCASE_REGEX.search(text):
            self.search_re_flags = re.RegexFlag(0)
        else:
            self.search_re_flags = re.IGNORECASE

        self.search_regex = re.compile(re.escape(self.search_text),
                                       self.search_re_flags)

    def new_search(self, text):
        """Start a new search for the given text."""
        self._set_search_regex(text)
        # Default search direction when hitting enter in the search bar.
        self.search_backwards()

    def disable_search_highlighting(self):
        self.log_pane.log_view.search_highlight = False

    def apply_filter(self, text=None):
        """Set a filter."""
        if not text:
            text = self.search_text
        self._set_search_regex(text)
        self.filter_text = text
        self.filter_regex = self.search_regex
        self.search_highlight = False

        self.filter_existing_logs_task = asyncio.create_task(
            self.filter_logs())

    async def filter_logs(self):
        """Filter"""
        # TODO(tonymd): Filter existing lines here.
        await asyncio.sleep(.3)

    def set_log_pane(self, log_pane: 'LogPane'):
        """Set the parent LogPane instance."""
        self.log_pane = log_pane

    def get_current_line(self):
        """Return the currently selected log event index."""
        return self.line_index

    def get_total_count(self):
        """Total size of the logs store."""
        return self.log_store.get_total_count()

    def clear_scrollback(self):
        """Hide log lines before the max length of the stored logs."""
        # TODO(tonymd): Should the LogStore be erased?

    def wrap_lines_enabled(self):
        """Get the parent log pane wrap lines setting."""
        if not self.log_pane:
            return False
        return self.log_pane.wrap_lines

    def toggle_follow(self):
        """Toggle auto line following."""
        self.follow = not self.follow
        if self.follow:
            self.scroll_to_bottom()

    def get_line_wrap_prefix_width(self):
        if self.wrap_lines_enabled():
            if self.log_pane.table_view:
                return self.log_store.table.column_width_prefix_total
            return self.log_store.longest_channel_prefix_width
        return 0

    def new_logs_arrived(self):
        # If follow is on, scroll to the last line.
        # TODO(tonymd): Filter new lines here.
        if self.follow:
            self.scroll_to_bottom()

        # Trigger a UI update
        self._update_prompt_toolkit_ui()

    def _update_prompt_toolkit_ui(self):
        """Update Prompt Toolkit UI if a certain amount of time has passed."""
        emit_time = time.time()
        # Has enough time passed since last UI redraw?
        if emit_time > self._last_ui_update_time + self._ui_update_frequency:
            # Update last log time
            self._last_ui_update_time = emit_time

            # Trigger Prompt Toolkit UI redraw.
            self.log_pane.application.redraw_ui()

    def get_cursor_position(self) -> Point:
        """Return the position of the cursor."""
        # This implementation is based on get_cursor_position from
        # prompt_toolkit's FormattedTextControl class.

        fragment = "[SetCursorPosition]"
        # If no lines were rendered.
        if not self._line_fragment_cache:
            return Point(0, 0)
        # For each line rendered in the last pass:
        for row, line in enumerate(self._line_fragment_cache):
            column = 0
            # For each style string and raw text tuple in this line:
            for style_str, text, *_ in line:
                # If [SetCursorPosition] is in the style set the cursor position
                # to this row and column.
                if fragment in style_str:
                    return Point(x=column +
                                 self.log_pane.get_horizontal_scroll_amount(),
                                 y=row)
                column += len(text)
        return Point(0, 0)

    def scroll_to_top(self):
        """Move selected index to the beginning."""
        # Stop following so cursor doesn't jump back down to the bottom.
        self.follow = False
        self.line_index = 0

    def scroll_to_bottom(self):
        """Move selected index to the end."""
        # Don't change following state like scroll_to_top.
        self.line_index = max(0, self.log_store.get_last_log_line_index())

    def scroll(self, lines):
        """Scroll up or down by plus or minus lines.

        This method is only called by user keybindings.
        """
        # If the user starts scrolling, stop auto following.
        self.follow = False

        # If scrolling to an index below zero, set to zero.
        new_line_index = max(0, self.line_index + lines)
        # If past the end, set to the last index of self.logs.
        if new_line_index >= self.log_store.get_total_count():
            new_line_index = self.log_store.get_last_log_line_index()
        # Set the new selected line index.
        self.line_index = new_line_index

    def scroll_to_position(self, mouse_position: Point):
        """Set the selected log line to the mouse_position."""
        # If auto following don't move the cursor arbitrarily. That would stop
        # following and position the cursor incorrectly.
        if self.follow:
            return

        cursor_position = self.get_cursor_position()
        if cursor_position:
            scroll_amount = cursor_position.y - mouse_position.y
            self.scroll(-1 * scroll_amount)

    def scroll_up_one_page(self):
        """Move the selected log index up by one window height."""
        lines = 1
        if self._window_height > 0:
            lines = self._window_height
        self.scroll(-1 * lines)

    def scroll_down_one_page(self):
        """Move the selected log index down by one window height."""
        lines = 1
        if self._window_height > 0:
            lines = self._window_height
        self.scroll(lines)

    def scroll_down(self, lines=1):
        """Move the selected log index down by one or more lines."""
        self.scroll(lines)

    def scroll_up(self, lines=1):
        """Move the selected log index up by one or more lines."""
        self.scroll(-1 * lines)

    def get_log_window_indices(self,
                               available_width=None,
                               available_height=None):
        """Get start and end index."""
        self._last_start_index = self._current_start_index
        self._last_end_index = self._current_end_index

        starting_index = 0
        ending_index = self.line_index

        self._window_width = self.log_pane.current_log_pane_width
        self._window_height = self.log_pane.current_log_pane_height
        if available_width:
            self._window_width = available_width
        if available_height:
            self._window_height = available_height

        # If render info is available we use the last window height.
        if self._window_height > 0:
            # Window lines are zero indexed so subtract 1 from the height.
            max_window_row_index = self._window_height - 1

            starting_index = max(0, self.line_index - max_window_row_index)
            # Use the current_window_height if line_index is less
            ending_index = max(self.line_index, max_window_row_index)

        if ending_index > self.log_store.get_last_log_line_index():
            ending_index = self.log_store.get_last_log_line_index()

        # Save start and end index.
        self._current_start_index = starting_index
        self._current_end_index = ending_index

        return starting_index, ending_index

    def render_table_header(self):
        """Get pre-formatted table header."""
        return self.log_store.render_table_header()

    def render_content(self) -> List:
        """Return log lines as a list of FormattedText tuples.

        This function handles selecting the lines that should be displayed for
        the current log line position and the given window size. It also sets
        the cursor position depending on which line is selected.
        """
        # Reset _line_fragment_cache ( used in self.get_cursor_position )
        self._line_fragment_cache = collections.deque()

        # Track used lines.
        total_used_lines = 0

        # If we have no logs add one with at least a single space character for
        # the cursor to land on. Otherwise the cursor will be left on the line
        # above the log pane container.
        if self.log_store.get_total_count() < 1:
            return [(
                '[SetCursorPosition]', '\n' * self._window_height
                # LogContentControl.mouse_handler will handle focusing the log
                # pane on click.
            )]

        # Get indicies of stored logs that will fit on screen.
        starting_index, ending_index = self.get_log_window_indices()

        # NOTE: Since range() is not inclusive use ending_index + 1.
        #
        # Build up log lines from the bottom of the window working up.
        #
        # From the ending_index to the starting index in reverse:
        for i in range(ending_index, starting_index - 1, -1):
            # Stop if we have used more lines than available.
            if total_used_lines > self._window_height:
                break

            # Grab the rendered log line using the table or standard view.
            line_fragments: StyleAndTextTuples = (
                self.log_store.table.formatted_row(self.log_store.logs[i])
                if self.log_pane.table_view else
                self.log_store.logs[i].get_fragments())

            # Get the width, height and remaining width.
            fragment_width = fragment_list_width(line_fragments)
            line_height = 1
            remaining_width = 0
            # Get the line height respecting line wrapping.
            if self.wrap_lines_enabled() and (fragment_width >
                                              self._window_width):
                line_height, remaining_width = (
                    pw_console.text_formatting.get_line_height(
                        fragment_width, self._window_width,
                        self.get_line_wrap_prefix_width()))

            # Keep track of how many lines are used.
            used_lines = line_height

            # Count the number of line breaks are included in the log line.
            line_breaks = self.log_store.logs[i].ansi_stripped_log.count('\n')
            used_lines += line_breaks

            # If this is the selected line apply a style class for highlighting.
            selected = i == self.line_index
            if selected:
                line_fragments = (
                    pw_console.text_formatting.fill_character_width(
                        line_fragments,
                        fragment_width,
                        self._window_width,
                        remaining_width,
                        self.wrap_lines_enabled(),
                        horizontal_scroll_amount=(
                            self.log_pane.get_horizontal_scroll_amount()),
                        add_cursor=True))

                # Apply the selected-log-line background color
                line_fragments = to_formatted_text(
                    line_fragments, style='class:selected-log-line')

            # Apply search term highlighting.
            if self.search_regex and self.search_highlight and (
                    self.search_regex.search(
                        self.log_store.logs[i].ansi_stripped_log)):
                line_fragments = self._highlight_search_matches(
                    line_fragments, selected)

            # Save this line to the beginning of the cache.
            self._line_fragment_cache.appendleft(line_fragments)
            total_used_lines += used_lines

        # Pad empty lines above current lines if the window isn't filled. This
        # will push the table header to the top.
        if total_used_lines < self._window_height:
            empty_line_count = self._window_height - total_used_lines
            self._line_fragment_cache.appendleft([('', '\n' * empty_line_count)
                                                  ])

        return pw_console.text_formatting.flatten_formatted_text_tuples(
            self._line_fragment_cache)

    def _highlight_search_matches(self, line_fragments, selected=False):
        """Highlight search matches in the current line_fragment."""
        line_text = fragment_list_to_text(line_fragments)
        exploded_fragments = explode_text_fragments(line_fragments)

        # Loop through each non-overlapping search match.
        for match in self.search_regex.finditer(line_text):
            for fragment_i in range(match.start(), match.end()):
                # Expand all fragments and apply the highlighting style.
                old_style, _text, *_ = exploded_fragments[fragment_i]
                if selected:
                    exploded_fragments[fragment_i] = (
                        old_style + ' class:search.current ',
                        exploded_fragments[fragment_i][1],
                    )
                else:
                    exploded_fragments[fragment_i] = (
                        old_style + ' class:search ',
                        exploded_fragments[fragment_i][1],
                    )
        return exploded_fragments