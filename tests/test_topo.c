#include <stdio.h>
#include "../include/topo.h"

int main() {
    topo_t t;
    
    topo_init(&t);
    topo_print(&t);
    topo_destroy();
    
    return 0;
}
