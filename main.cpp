#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include "httpd.h"

using namespace std;

void usage(char * argv0)
{
    cerr << "Usage: " << argv0 << " listen_port docroot_dir" << endl;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    long int port = strtol(argv[1], NULL, 10);

    if (errno == EINVAL || errno == ERANGE) {
        usage(argv[0]);
        return 2;
    }

    if (port <= 0 || port > USHRT_MAX) {
        cerr << "Invalid port: " << port << endl;
        return 3;
    }

    string doc_root = argv[2];

    try {
        start_httpd(port, doc_root);
    } catch (runtime_error &e) {
        std::cout << e.what() << "\r\n";
        return -1;
    }

    return 0;
}
