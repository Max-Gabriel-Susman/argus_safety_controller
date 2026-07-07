#include "xil_printf.h"

int main(void)
{
    xil_printf("Hello World from Arty Z7!\r\n");

    while (1) {
        // stay here
    }

    return 0;
}