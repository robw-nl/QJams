#ifndef QJAMS_SESSION_H
#define QJAMS_SESSION_H

/**
 * @brief Saves the current multitrack session to a monolithic binary .qjams file.
 * @param filepath Absolute path to the destination file.
 * @return 0 on success, or a negative error code.
 */
int save_qjams_session(const char* filepath);

/**
 * @brief Loads a multitrack session from a .qjams file, safely mapping it into the engine.
 * @param filepath Absolute path to the source file.
 * @return 0 on success, or a negative error code.
 */
int load_qjams_session(const char* filepath);

#endif // QJAMS_SESSION_H
