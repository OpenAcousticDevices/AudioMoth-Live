/****************************************************************************
 * xdirectory.c
 * openacousticdevices.info
 * March 2023
 *****************************************************************************/

#include "xdirectory.h"

#if defined(_WIN32) || defined(_WIN64)

    #include <io.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    
    bool Directory_exists(const char *path) {

        if (_access(path, 0 ) == 0) {

            struct stat status;
            stat(path, &status);

            return (status.st_mode & S_IFDIR) != 0;
        
        }
    
        return false;

    }

#else

    #include <dirent.h>
    #include <stdlib.h>

    bool Directory_exists(const char *path) {

        DIR *dir = opendir(path);

        return dir != NULL;

    }

#endif