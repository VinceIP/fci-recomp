/* A tiny DLL whose last section is a discardable INIT holding fci_kinit: the game (004AD2E4) reads the
 * kernel's PE header and expects that layout, so fci_main.c copies this DLL's header to 80010000. */
__attribute__((section("INIT"))) __declspec(dllexport) unsigned fci_kinit(volatile unsigned *ran)
{
    *ran = 0x54494E49u; /* "INIT" */
    return 0x54494E49u;
}
