/* false - fail, and do nothing else. */
#include "types.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: false [ignored command line arguments]\n"
               "Exit with a status code indicating failure.\n");
        return 1;
    }
    return 1;
}
