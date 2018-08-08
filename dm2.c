/*
 * dm2.c  -  Mixman DM2 stateful MIDI driver
 *
 *
 * Copyright (C) 2007-2008 Jan Jockusch (jan@jockusch.de)
 * Copyright (C) 2006-2008 Andre Roth <lynx@netlabs.org>
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * $Id: dm2.c,v 1.53 2008/03/04 10:55:10 jan Exp $
 *
 */

/*
 * TODO
 *
 * Note on / note off on primary and secondary beat: reset LED position,
 * then advance one position on every secondary beat.
 * CC on a channel: light the LEDs in a VU meter pattern.
 *
 * MIDI configuration: Use a system exclusive (SysEx) block to program
 * all keys and buttons. Use reset (0xff) to get the default mode.
 *
 * Emulate BCD 3000 feature of calling all controller settings with one
 * SysEx call.
 * 
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#include <sound/core.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>

#include "dm2.h"

static int index = SNDRV_DEFAULT_IDX1;	/* Index 0-MAX */
static char *id = SNDRV_DEFAULT_STR1;	/* ID for this card */

module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for DM2 MIDI controller.");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for DM2 MIDI controller.");

static struct usb_driver dm2_driver;

// Make kernel version check
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
#  warning This driver will not compile for kernels older than 2.6.22
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
#  warning Please make sure your kernel is patched with linux-lowspeedbulk.patch
#  define USE_BULK_SNDPIPE 1
#endif
// Kernel API compatibility
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
static inline
int snd_card_create(int idx, const char *id,
		    struct module *module, int extra_size,
		    struct snd_card **card_ret)
{
	*card_ret = snd_card_new(idx, id, module, extra_size);
	if (!(*card_ret)) return -1;
	return 0;
}
#endif

#define err(format, arg...) printk(KERN_ERR KBUILD_MODNAME ": " format "\n" , ## arg)
#define info(format, arg...) printk(KERN_INFO KBUILD_MODNAME ": " format "\n" , ## arg)


static void dm2_slider_reset(struct dm2slider *slider, u8 value)
{
	slider->pos = value;
	slider->mid = value;
	slider->min = value - slider->dead - 1;
	slider->max = (slider->max) ? value + slider->dead + 1 : 0;
	slider->midival = 64;
}

static void dm2_slider_set(struct dm2slider *slider, u8 value)
{
	if (value < slider->min) slider->min = value;
	if (slider->max && (value > slider->max)) slider->max = value;
	slider->pos = value;
}

static int dm2_slider_get(struct dm2slider *slider)
{
	int value;
	u8 max = slider->max;

	if (!max) max = (slider->mid<<1) - slider->min;
	if (slider->pos < slider->mid) {
		value = ((slider->pos - slider->min)*64 /
			 (slider->mid - slider->dead - slider->min));
		if (value > 64) value = 64;
	} else {
		value = (127 - (max - slider->pos)*63 /
			 (max - slider->dead - slider->mid));
		if (value < 64) value = 64;
	}
	if (value < 0) value = 0;
	if (value > 127) value = 127;
	return value;
}

static void dm2_slider_update(struct usb_dm2 *dev, struct dm2slider *slider, u8 prev, u8 curr)
{
	int value;
	
	dm2_slider_set(slider, curr);
	value = dm2_slider_get(slider);
	if (value == slider->midival) return;
	dm2_midi_send(dev, 0xb0, slider->param, value);
	slider->midival = value;
	return;
}

static void dm2_leds_update(struct dm2 *dm2, u8 note, u8 vel)
{
	u16 leds;
	if (note >= 16) {
		return;
	}

	leds = *((u16*)(dm2->leds));

	*((u16*)(dm2->leds)) = (vel ? leds | (1 << note) : leds & ~(1 << note));
}

static void dm2_leds_send(struct usb_dm2 *dev)
{
	struct dm2* dm2 = &dev->dm2;
	if (memcmp(dm2->leds, dm2->prev_leds, sizeof(dm2->leds))) {
		dm2_set_leds(dev, dm2->leds[0], dm2->leds[1]);
		memcpy(dm2->prev_leds, dm2->leds, sizeof(dm2->leds));
	}
}


/* Main event handler */

static void dm2_tasklet(unsigned long arg)
{
	struct usb_dm2 *dev;
	u8 curr[10], prev[10], i;
	unsigned long flags;

	dev = (struct usb_dm2 *)arg;

	spin_lock_irqsave(&dev->lock, flags);
	memcpy(curr, dev->dm2.curr_state, 10*sizeof(u8));
	spin_unlock_irqrestore(&dev->lock, flags);

	memcpy(prev, dev->dm2.prev_state, 10*sizeof(u8));

	// Update LEDs
	dm2_leds_send(dev);

	if(!memcmp(prev, curr, sizeof(curr))) {
		return;
	}

	// Bytes 0-3: Handle buttons
	u32 button_diff = *(u32*)prev ^ *(u32*)curr;
	for (i = 0; i < 32; ++i) {
		if (button_diff & (1 << (i))) {
			dm2_midi_send(dev, 0x90, i, *(u32*)curr & (1 << (i)) ? 0x7f : 0x00);
		}
	}

	// bytes 5, 6, 7: handle sliders.
	if (curr[5] != prev[5]) dm2_slider_update(dev, &(dev->dm2.sliders[0]), prev[5], curr[5]);
	if (curr[6] != prev[6]) dm2_slider_update(dev, &(dev->dm2.sliders[1]), prev[6], curr[6]);
	if (curr[7] != prev[7]) dm2_slider_update(dev, &(dev->dm2.sliders[2]), prev[7], curr[7]);

	// bytes 8, 9: handle wheels.
	//if (curr[8] || prev[8]) dm2_wheel_turn(dev, &(dev->dm2.wheels[0]), curr[8]);
	//if (curr[9] || prev[9]) dm2_wheel_turn(dev, &(dev->dm2.wheels[1]), curr[9]);

	memcpy(dev->dm2.prev_state, curr, 10*sizeof(u8));
}



/* URB writing interface */

static ssize_t dm2_write(struct usb_dm2 *dev, const char *data, size_t count);
static void dm2_set_leds(struct usb_dm2 *dev, u8 left, u8 right)
{
	char data[4] = { 0xff, 0xff, 0xff, 0xff };
	data[0] ^= right; data[1] ^= left;
	dm2_write(dev, data, 4);
}

/* Basic interpretation of received URBs */

static void dm2_update_status(struct usb_dm2 *dev, u8 *buf, int length)
{
	// ATTENTION: Called in interrupt context!
	int i;
	unsigned long flags;

	if (length != 10) {
		err("Unexpected URB length!");
		return;
	}

	// Invert X joystick axis.
	buf[5] = ~buf[5];

	// Slider initialization with fancy LED blinking.
	if (dev->dm2.initialize==38) dm2_set_leds(dev, 0xaa, 0x55);
	if (dev->dm2.initialize==25) dm2_set_leds(dev, 0x55, 0xaa);
	if (dev->dm2.initialize==12) dm2_set_leds(dev, 0xff, 0xff);
	if (dev->dm2.initialize==1)  dm2_set_leds(dev, 0x00, 0x00);
	if (dev->dm2.initialize && (!--dev->dm2.initialize)) {
		for (i=0; i<3; i++) dm2_slider_reset(&(dev->dm2.sliders[i]), buf[i+5]);
		dm2_set_leds(dev, 0, 0);
	}

	// Nothing works until initialization is complete!
	if (dev->dm2.initialize) return;	

	// Transfer latest transmission into dm2 structure.
	spin_lock_irqsave(&dev->lock, flags);
	memcpy(dev->dm2.curr_state, buf, 10*sizeof(u8));
	spin_unlock_irqrestore(&dev->lock, flags);

	// Trigger further processing.
	tasklet_schedule(&dev->dm2midi.tasklet);

	return;
}


/* Initialize DM2 structure */

static void dm2_internal_init(struct dm2 *dm2)
{
	memset(dm2, 0, sizeof(&dm2));
	dm2->initialize = 50;
	return;
}


/* MIDI processing */
static void dm2_midi_process(struct usb_dm2 *dev, u8 input[3])
{
	switch (input[0]) {
	case 0xb0:
		dm2_leds_update(&dev->dm2, input[1], input[2]);
	}
}

/* Midi functions */

static int dm2_midi_input_open(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
 	dev->dm2midi.input = substream;
	/* Reset the current status */
	dev->dm2midi.out_rstatus = 0;
	/* increment our usage count for the device */
	kref_get(&dev->kref);
	return 0;
}

static int dm2_midi_input_close(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	dev->dm2midi.input = NULL;
	/* decrement the count on our device */
	kref_put(&dev->kref, dm2_delete);
	return 0;
}

static int dm2_midi_output_open(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	dev->dm2midi.output = substream;
	/* increment our usage count for the device */
	kref_get(&dev->kref);
	return 0;
}

static int dm2_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	/* decrement the count on our device */
	kref_put(&dev->kref, dm2_delete);
	return 0;
}

static void dm2_midi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	if (up)
		dev->dm2midi.input_triggered = 1;
	else
		dev->dm2midi.input_triggered = 0;
	// Should reschedule a tasklet which does snd_rawmidi_receive(substream, data, len) ?
}

static void dm2_midi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	u8 input[3];

	while (snd_rawmidi_transmit(substream, input, 3) == 3) {
		dm2_midi_process(dev, input);
	}
}

static struct snd_rawmidi_ops dm2_midi_output = {
	.open =		dm2_midi_output_open,
	.close =	dm2_midi_output_close,
	.trigger =	dm2_midi_output_trigger,
};

static struct snd_rawmidi_ops dm2_midi_input = {
	.open = 	dm2_midi_input_open,
	.close =	dm2_midi_input_close,
	.trigger =	dm2_midi_input_trigger,
};


static void dm2_midi_send(struct usb_dm2 *dev, u8 cmd, u8 param, u8 value)
{
	unsigned char midimsg[3] = { cmd, param, value };
	if (!dev->dm2midi.input) return;
	midimsg[0] += dev->dm2midi.chan;
	// Use running status
	if (midimsg[0] == dev->dm2midi.out_rstatus)
		snd_rawmidi_receive(dev->dm2midi.input, midimsg+1, 2);
	else
		snd_rawmidi_receive(dev->dm2midi.input, midimsg, 3);
	dev->dm2midi.out_rstatus = midimsg[0];
}


static int  dm2_midi_init(struct usb_dm2 *dev)
{
	struct snd_rawmidi *rmidi;
	struct snd_card *card;
	int err;

	tasklet_init(&dev->dm2midi.tasklet, dm2_tasklet, (unsigned long)dev );

	if (snd_card_new(&dev->udev->dev, index, id, THIS_MODULE, 0, &card) < 0) {
		printk("%s snd_card_create failed\n", __FUNCTION__);
		return -ENOMEM;
	}
	dev->dm2midi.card = card;
	if ((err = snd_rawmidi_new(dev->dm2midi.card, "Mixman DM2", 1, 1, 1, &rmidi)) < 0) {
		printk("%s snd_rawmidi_new failed\n", __FUNCTION__);
		return err;
	}
	strcpy(rmidi->name, "Mixman DM2");
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &dm2_midi_output);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &dm2_midi_input);
	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = dev;
	dev->dm2midi.rmidi = rmidi;

	if ((err = snd_card_register(dev->dm2midi.card)) < 0) {
		printk( "%s snd_card_register failed\n", __FUNCTION__);
		snd_card_free(dev->dm2midi.card);
		return err;
	}

	// Variables
	dev->dm2midi.chan = 0;

	return 0;
}


static void dm2_midi_destroy(struct usb_dm2 *dev)
{
	if (dev->dm2midi.card) {
                snd_card_free(dev->dm2midi.card);
		dev->dm2midi.card = NULL;
	}
}


/* End of MIDI functions */



/* Generic USB driver section below. Only hook new functions in, do not edit a lot! */


static void dm2_write_int_callback(struct urb *urb)
{
	struct usb_dm2 *dev;

	dev = (struct usb_dm2 *)urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status &&
	    !(urb->status == -ENOENT ||
	      urb->status == -ECONNRESET ||
	      urb->status == -ESHUTDOWN)) {
		err("%s - nonzero write status received: %d",
		    __FUNCTION__, urb->status);
	}
	/* Unlock collision detector */
	dev->output_failed = 0;
	up(&dev->limit_sem);
}


static ssize_t dm2_write(struct usb_dm2 *dev, const char *data, size_t count)
{
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min(count, (size_t)MAX_TRANSFER);
	unsigned long flags;

	/* If there's trouble with output (on <=2.6.22 without patch),
	 * we bail out immediately. */

	/* This doubles as a collision preventer... */
	if (dev->output_failed) goto exit;

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/* limit the number of URBs in flight to stop a user from using up all RAM */
	if (down_interruptible(&dev->limit_sem)) {
		retval = -ERESTARTSYS;
		goto exit;
	}

	urb = dev->int_out_urb;
	buf = dev->int_out_buffer;

	memcpy(buf, data, writesize);

	/* this lock makes sure we don't submit URBs to gone devices */
	spin_lock_irqsave(&dev->lock, flags);
	if (!dev->interface) {		/* disconnect() was called */
		spin_unlock_irqrestore(&dev->lock, flags);
		retval = -ENODEV;
		goto error;
	}

	/* send the data out the int port */
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	spin_unlock_irqrestore(&dev->lock, flags);

	if (retval) {
		err("%s - failed submitting write urb, error %d", __FUNCTION__, retval);
		if (retval == -EINVAL) {
			dev->output_failed = 1;
			info("Your kernel cannot transmit data to the DM2.");
			info("The driver will still work, but there will be no LED output.");
			info("To make the LEDs work on 2.6.22, please apply the kernel patch that came with this driver!");
		}
		goto error;
	}

	/* Collision prevention */
	dev->output_failed = 1;

	return writesize;

error:
	up(&dev->limit_sem);

exit:
	if (retval) printk("%s - failed to write urb, error %d\n", __FUNCTION__, retval);
	return retval;
}


static void dm2_read_int_callback(struct urb *urb)
{
	// ATTENTION: Called in interrupt context!
	struct usb_dm2 *dev = urb->context;
  
	if (urb->status == 0) {
		dm2_update_status(dev, urb->transfer_buffer, urb->actual_length);
	}
	if (urb->status != -ENOENT && urb->status != -ECONNRESET) {
		urb->dev = dev->udev;
		usb_submit_urb(urb, GFP_ATOMIC);
	}
}

static int dm2_setup_writer(struct usb_dm2 *dev) {
	int bufsize = 4;
	void *buf = NULL;
	struct urb *urb = NULL;

	buf = kmalloc(bufsize, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
  
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		kfree(buf);
		return -ENOMEM;
	}
#ifdef USE_BULK_SNDPIPE
	// Compatibility code for older kernels:
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->int_out_endpointAddr),
			  buf, bufsize, dm2_write_int_callback, dev);
#else
	usb_fill_int_urb(urb, dev->udev,
			 usb_sndintpipe(dev->udev, dev->int_out_endpointAddr),
			 buf, bufsize, dm2_write_int_callback, dev, 10);
#endif
	// urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP || URB_ZERO_PACKET;

	dev->int_out_urb = urb;
	dev->int_out_buffer = buf;

	return 0;
}

static int dm2_setup_reader(struct usb_dm2 *dev) {
	int bufsize = 32;
	int retval;
	void *buf = NULL;
	struct urb *urb = NULL;

	buf = kmalloc(bufsize, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
  
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		kfree(buf);
		return -ENOMEM;
	}
	usb_fill_int_urb(urb, dev->udev,
			 usb_rcvintpipe(dev->udev, dev->int_in_endpointAddr ),
			 buf, bufsize,
			 dm2_read_int_callback, dev, dev->int_in_interval);
	dev->int_in_urb = urb;
	retval = usb_submit_urb(urb, GFP_KERNEL);
	if (retval) {
		kfree(buf);
		return retval;
	}
	return 0;
}

static void dm2_delete(struct kref *kref)
{
	struct usb_dm2 *dev = to_dm2_dev(kref);
	// struct urb *urb;

	usb_put_dev(dev->udev);
	kfree(dev->int_in_buffer);

	/* XXX  Handling correct? */
	kfree(dev->int_out_buffer);
	// urb = dev->int_out_urb;
	// usb_free_urb(urb);

	kfree(dev);
}

static int dm2_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_dm2 *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err("Out of memory");
		goto error;
	}
	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	spin_lock_init(&dev->lock);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;

	/* set up the endpoint information */
	/* use only the first int-in and int-out endpoints */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;
		if (!dev->int_in_endpointAddr &&
		    usb_endpoint_is_int_in(endpoint)) {
			/* we found a int in endpoint */
			buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
			dev->int_in_size = buffer_size;
			dev->int_in_endpointAddr = endpoint->bEndpointAddress;
			dev->int_in_interval = endpoint->bInterval;
			dev->int_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if (!dev->int_in_buffer) {
				err("Could not allocate int_in_buffer");
				goto error;
			}
		}
#ifdef USE_BULK_SNDPIPE
		// Compatibility code for older kernels:
		if (!dev->int_out_endpointAddr &&
		    usb_endpoint_is_bulk_out(endpoint)) {
			/* we found a bulk out endpoint */
			dev->int_out_endpointAddr = endpoint->bEndpointAddress;
		}
#else
		if (!dev->int_out_endpointAddr &&
		    usb_endpoint_is_int_out(endpoint)) {
			/* we found an int out endpoint */
			dev->int_out_endpointAddr = endpoint->bEndpointAddress;
		}
#endif
	}
	if (!(dev->int_in_endpointAddr && dev->int_out_endpointAddr)) {
		err("Could not find both int-in and int-out endpoints");
		goto error;
	}

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	retval = dm2_setup_writer(dev);
	if (retval) {
		err("Problem setting up the writer.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	retval = dm2_setup_reader(dev);
	if (retval) {
		err("Problem setting up the reader.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	retval = dm2_midi_init(dev);
	if (retval) {
		err("Problem setting up MIDI.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	dm2_internal_init(&(dev->dm2));


	info("Mixman DM2 device now attached.");
	return 0;

error:
	if (dev)
		/* this frees allocated memory */
		kref_put(&dev->kref, dm2_delete);
	return retval;
}

static void dm2_disconnect(struct usb_interface *interface)
{
	struct usb_dm2 *dev;
	unsigned long flags;

	dev = usb_get_intfdata(interface);

	/* prevent dm2_open() from racing dm2_disconnect() */
	spin_lock_irqsave(&dev->lock, flags);

	usb_set_intfdata(interface, NULL);
	/* prevent more I/O from starting */
	dev->interface = NULL;

	spin_unlock_irqrestore(&dev->lock, flags);

	/* decrement our usage count */
	kref_put(&dev->kref, dm2_delete);

	dm2_midi_destroy(dev);

	info("Mixman DM2 now disconnected");
}

static struct usb_driver dm2_driver = {
	.name =		"Mixman DM2",
	.probe =	dm2_probe,
	.disconnect =	dm2_disconnect,
	.id_table =	dm2_table,
	.supports_autosuspend = 0,
};

static int __init usb_dm2_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&dm2_driver);
	if (result)
		err("usb_register failed. Error number %d", result);

	return result;
}

static void __exit usb_dm2_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&dm2_driver);
}

module_init(usb_dm2_init);
module_exit(usb_dm2_exit);

MODULE_LICENSE("GPL");
