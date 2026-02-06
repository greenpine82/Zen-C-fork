#ifndef REPL_OS_H
#define REPL_OS_H

/**
 * @brief Enable raw mode for the terminal.
 * This disables canonical mode (line buffering) and echo.
 * Registers an atexit handler to restore the terminal state.
 */
void repl_enable_raw_mode(void);

/**
 * @brief Disable raw mode, restoring original terminal settings.
 */
void repl_disable_raw_mode(void);

/**
 * @brief Read a single character from stdin.
 * Handles platform differences (read() vs _getch() etc.)
 * @param c Pointer to char to store result.
 * @return 1 on success, 0 on EOF/Error.
 */
int repl_read_char(char *c);

/**
 * @brief Get terminal window size.
 * @param rows Pointer to store rows.
 * @param cols Pointer to store columns.
 * @return 0 on success, -1 on failure.
 */
int repl_get_window_size(int *rows, int *cols);

#endif // REPL_OS_H
