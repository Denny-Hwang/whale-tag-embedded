//-----------------------------------------------------------------------------
// Minimal pigpio API surface for host-side unit tests.
// Resolved instead of the real <pigpio.h> via `-I tests/fakes` in Test.mk.
// Implementations are provided by the test that links the module under test
// (e.g. tests/src/cetiTagApp/recovery.test.c scripts the serial functions).
//-----------------------------------------------------------------------------
#ifndef FAKE_PIGPIO_H
#define FAKE_PIGPIO_H

#define PI_INIT_FAILED -1

int serOpen(char *sertty, unsigned baud, unsigned serFlags);
int serClose(unsigned handle);
int serWrite(unsigned handle, char *buf, unsigned count);
int serRead(unsigned handle, char *buf, unsigned count);
int serDataAvailable(unsigned handle);

#endif // FAKE_PIGPIO_H
