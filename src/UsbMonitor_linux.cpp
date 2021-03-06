/******************************************************************************
 **  Copyright (c) Raoul Hecky. All Rights Reserved.
 **
 **  Calaos is free software; you can redistribute it and/or modify
 **  it under the terms of the GNU General Public License as published by
 **  the Free Software Foundation; either version 3 of the License, or
 **  (at your option) any later version.
 **
 **  Calaos is distributed in the hope that it will be useful,
 **  but WITHOUT ANY WARRANTY; without even the implied warranty of
 **  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **  GNU General Public License for more details.
 **
 **  You should have received a copy of the GNU General Public License
 **  along with Foobar; if not, write to the Free Software
 **  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 **
 ******************************************************************************/
#include "UsbMonitor_linux.h"
#include <QSocketNotifier>

static inline QString fmtId(int id)
{
    return QString("0x%1").arg(id, 1, 16);
}

int libusb_device_add_cb(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
    Q_UNUSED(event);
    Q_UNUSED(ctx);
    UsbMonitor_linux *um = reinterpret_cast<UsbMonitor_linux *>(user_data);
    struct libusb_device_descriptor desc;

    int rc = libusb_get_device_descriptor(dev, &desc);

    if (rc != LIBUSB_SUCCESS)
        qWarning() << "Error getting device descriptor";
    else
    {
        emit um->usbDeviceAdded();
        qDebug().noquote() << "Device added: " << fmtId(desc.idVendor) << "-" << fmtId(desc.idProduct);
    }

    return 0;
}

int libusb_device_del_cb(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
    Q_UNUSED(event);
    Q_UNUSED(ctx);
    UsbMonitor_linux *um = reinterpret_cast<UsbMonitor_linux *>(user_data);
    struct libusb_device_descriptor desc;
    int rc;

    rc = libusb_get_device_descriptor(dev, &desc);
    qDebug() << rc;
    if (rc != LIBUSB_SUCCESS)
        qWarning() << "Error getting device descriptor";
    else
    {
        qDebug().noquote() << "Device removed: " << fmtId(desc.idVendor) << "-" << fmtId(desc.idProduct);
        emit um->usbDeviceRemoved();
    }
    return 0;
}

void libusb_fd_add_cb(int fd, short, void *user_data)
{
    UsbMonitor_linux * um = reinterpret_cast<UsbMonitor_linux *>(user_data);
    QMetaObject::invokeMethod(um, "createSocketMonitor",
                              Qt::QueuedConnection,
                              Q_ARG(int , fd));
}

void libusb_fd_del_cb(int fd, void *user_data)
{
     UsbMonitor_linux * um = reinterpret_cast<UsbMonitor_linux *>(user_data);
     QMetaObject::invokeMethod(um, "releaseSocketMonitor",
                               Qt::QueuedConnection,
                               Q_ARG(int , fd));
}

UsbMonitor_linux::UsbMonitor_linux()
{
    qRegisterMetaType<struct libusb_transfer *>();

    int err = libusb_init(&usb_ctx);
    if (err < 0 || !usb_ctx)
        qWarning() << "Failed to initialise libusb: " << libusb_error_name(err);

    if (!libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG))
        qDebug() << "libusb Hotplug capabilites are not supported on this platform";

    libusb_set_debug(usb_ctx, LIBUSB_LOG_LEVEL_ERROR);
}

void UsbMonitor_linux::start()
{
    attachCallbacks();
   // libusb_set_debug(usb_ctx, 4);
}

void UsbMonitor_linux::stop()
{
    relaseCallbacks();
}

void UsbMonitor_linux::handleEvents()
{
    if (libusb_try_lock_events(usb_ctx) == 0)
    {
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        libusb_handle_events_locked(usb_ctx, &timeout);
        libusb_unlock_events(usb_ctx);
    }
}

void UsbMonitor_linux::attachCallbacks()
{
    int err;
    err = libusb_hotplug_register_callback(usb_ctx,
                                           LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
                                           (libusb_hotplug_flag)0,
                                           MOOLTIPASS_VENDORID,
                                           MOOLTIPASS_PRODUCTID,
                                           LIBUSB_HOTPLUG_MATCH_ANY,
                                           libusb_device_add_cb,
                                           this,
                                           &cbaddhandle);
    if (err != LIBUSB_SUCCESS)
        qWarning() << "Failed to register LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED callback";

    err = libusb_hotplug_register_callback(usb_ctx,
                                           LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                           (libusb_hotplug_flag)0,
                                           MOOLTIPASS_VENDORID,
                                           MOOLTIPASS_PRODUCTID,
                                           LIBUSB_HOTPLUG_MATCH_ANY,
                                           libusb_device_del_cb,
                                           this,
                                           &cbdelhandle);
    if (err != LIBUSB_SUCCESS)
        qWarning() << "Failed to register LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT callback";

    libusb_set_pollfd_notifiers(usb_ctx, libusb_fd_add_cb, libusb_fd_del_cb, this);

    auto fds = libusb_get_pollfds(usb_ctx);
    for (;fds && *fds; fds ++)
        createSocketMonitor((*fds)->fd);
}

void UsbMonitor_linux::relaseCallbacks()
{
    libusb_hotplug_deregister_callback(usb_ctx, cbaddhandle);
    libusb_hotplug_deregister_callback(usb_ctx, cbdelhandle);

    qDeleteAll(m_fdMonitors.values());
    m_fdMonitors.clear();
}

UsbMonitor_linux::~UsbMonitor_linux()
{
    relaseCallbacks();
    libusb_exit(usb_ctx);
}

void UsbMonitor_linux::createSocketMonitor(int fd)
{
    if (!m_fdMonitors.contains(fd))
    {
        QSocketNotifier* sn = new QSocketNotifier(fd, QSocketNotifier::Read);
        connect(sn, &QSocketNotifier::activated, this, &UsbMonitor_linux::handleEvents);
        m_fdMonitors[fd] = sn;
    }
}

void UsbMonitor_linux::releaseSocketMonitor(int fd)
{
    if (m_fdMonitors.contains(fd))
    {
        QSocketNotifier* sn = m_fdMonitors[fd];
        m_fdMonitors.remove(fd);
        sn->disconnect();
        sn->deleteLater();
    }
}
