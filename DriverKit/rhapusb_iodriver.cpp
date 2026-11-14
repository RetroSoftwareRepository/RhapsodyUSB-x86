/*
 * rhapusb_iodriver.cpp -- Rhapsody DriverKit wrapper.
 *
 * On Rhapsody DR2 there is no kext in the modern sense: no IOKit, no
 * IOUSBFamily, no Info.plist personality matching and no kextd.  The kernel
 * loader loads a Mach-O module and calls into a class derived from
 * DriverKit's IODriver.  This file is that class; all real work lives in the
 * plain-C stack it calls.
 *
 * The three places where DR2's DriverKit may disagree with this file are
 * marked ADAPT.  Check them against <DriverKit/IODriver.h> in your DDK --
 * this wrapper is 60 lines precisely so that this is cheap.
 */

#include <DriverKit/IODevice.h>
#include <DriverKit/IODriver.h>
#include <DriverKit/IOLib.h>

extern "C" {
int  rhap_usb_init(void);
void rhap_usb_shutdown(void);
int  rhap_usb_is_started(void);
}

class RhapsodyUSB : public IODriver
{
public:
    /* ADAPT: DR2 spells these init/free/start/stop on IODriver. */
    virtual bool init(IOService *provider);
    virtual void free(void);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);

private:
    bool fStarted;
};

bool RhapsodyUSB::init(IOService *provider)
{
    if (!IODriver::init(provider))
        return false;
    fStarted = false;
    return true;
}

bool RhapsodyUSB::start(IOService *provider)
{
    int rc;

    if (!IODriver::start(provider))
        return false;

    rc = rhap_usb_init();
    if (rc != 0) {
        IOLog("RhapsodyUSB: no usable UHCI controller (%d)\n", rc);
        return false;       /* probe fails, another driver may claim us */
    }

    fStarted = true;
    IOLog("RhapsodyUSB: up\n");
    return true;
}

void RhapsodyUSB::stop(IOService *provider)
{
    if (fStarted) {
        rhap_usb_shutdown();
        fStarted = false;
    }
    IODriver::stop(provider);
}

void RhapsodyUSB::free(void)
{
    if (fStarted) {
        rhap_usb_shutdown();
        fStarted = false;
    }
    IODriver::free();
}

/*
 * ADAPT: the DR2 kernel needs to know this module exists.  Depending on the
 * DR2 build that means an entry in the driver personality table (the
 * /usr/lib/DriverKit/*.driver or the kernel's driver config file) naming
 * class RhapsodyUSB, or the module being linked into the kernel itself.
 * There is no kextd and no Info.plist matching on DR2; see README.
 */
