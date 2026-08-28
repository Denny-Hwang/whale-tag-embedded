//-----------------------------------------------------------------------------
// Minimal pigpio API surface for host-side unit tests.
// Resolved instead of the real <pigpio.h> via `-I tests/fakes` in Test.mk.
// Implementations are provided by the test that links the module under test
// (e.g. tests/src/cetiTagApp/recovery.test.c scripts the serial functions).
//-----------------------------------------------------------------------------
#ifndef FAKE_PIGPIO_H
#define FAKE_PIGPIO_H

#define PI_INIT_FAILED -1
#define PI_NO_HANDLE -2005

// serial (implemented by the test that scripts them, e.g. recovery.test.c)
int serOpen(char *sertty, unsigned baud, unsigned serFlags);
int serClose(unsigned handle);
int serWrite(unsigned handle, char *buf, unsigned count);
int serRead(unsigned handle, char *buf, unsigned count);
int serDataAvailable(unsigned handle);

// i2c (implemented by tests/fakes/pigpio.fake.c as a scriptable register map)
int i2cOpen(unsigned i2cBus, unsigned i2cAddr, unsigned i2cFlags);
int i2cClose(unsigned handle);
int i2cWriteByte(unsigned handle, unsigned bVal);
int i2cReadByteData(unsigned handle, unsigned i2cReg);
int i2cWriteByteData(unsigned handle, unsigned i2cReg, unsigned bVal);
int i2cReadWordData(unsigned handle, unsigned i2cReg);
int i2cWriteWordData(unsigned handle, unsigned i2cReg, unsigned wVal);
int i2cReadDevice(unsigned handle, char *buf, unsigned count);

#endif // FAKE_PIGPIO_H
