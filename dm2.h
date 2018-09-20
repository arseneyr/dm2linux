/*
 * dm2.h  -  Mixman DM2 stateful MIDI driver header file
 *
 *
 * Copyright (C) 2007-2008 Jan Jockusch (jan@jockusch.de)
 * Copyright (C) 2006-2007 Andre Roth <lynx@netlabs.org>
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * $Id: dm2.h,v 1.15 2008/01/19 18:48:28 jan Exp $
 *
 */

struct dm2midi {
	struct snd_card			*card;
	struct snd_rawmidi		*rmidi;
	struct snd_rawmidi_substream	*input;
	struct snd_rawmidi_substream	*output;

	struct tasklet_struct		tasklet;
	int				input_triggered;

	u8		   	chan;		/* MIDI channel */
	u8			out_rstatus;	/* MIDI Running status reminder */
};


struct dm2slider {
	u8			pos;		/* Current position */
	u8			min, max, mid;	/* Values for auto-calibration */
	u8			dead;		/* Dead zone width in slider units */
	u8			param;
	u8			midival;
};

struct dm2wheel {
	u8			number;
	s8			direction;
};


#define DM2_MIDINDEX 3
#define DM2_MIDMASK 0x02
#define DM2_CLR 0x08
#define DM2_MID(v) (((v)&DM2_MIDMASK)<<2)


struct dm2 {
	u8			prev_state[10];
	u8			curr_state[10];
	struct dm2midi dm2midi;
	struct dm2slider	sliders[3];
	struct dm2wheel 	wheels[2];
	int			initialize;	/* Signals that the pots have to be initalized */
	u8 leds[2];
	u8 prev_leds[2];
};



/* Vendor and Product ID of the Mixman DM2 */
#define USB_DM2_VENDOR_ID	0x0665
#define USB_DM2_PRODUCT_ID	0x0301

/* table of devices that work with this driver */
static struct usb_device_id dm2_table [] = {
	{ USB_DEVICE(USB_DM2_VENDOR_ID, USB_DM2_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, dm2_table);


#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8


/* Structure to hold all of our device specific stuff */
struct usb_dm2 {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	unsigned char           *int_in_buffer;		/* the buffer to receive data */
	size_t			int_in_size;		/* the size of the receive buffer */
	__u8			int_in_endpointAddr;	/* the address of the int in endpoint */
	__u8			int_out_endpointAddr;	/* the address of the int/bulk out endpoint */
	int			output_failed;		/* flag which indicates an unpatched kernel */
	struct kref		kref;
	struct urb		*int_in_urb;
	int			int_in_interval;

	struct urb		*int_out_urb;		/* output URB */
	unsigned char           *int_out_buffer;	/* the buffer to send data */

	struct dm2		dm2;
	struct dm2midi          dm2midi;
	spinlock_t		lock;			/* To protect tasklet from irq handler */
};
#define to_dm2_dev(d) container_of(d, struct usb_dm2, kref)


static void dm2_midi_send(struct usb_dm2 *, u8, u8, u8);
static void dm2_set_leds(struct usb_dm2 *, u8, u8);

static void dm2_delete(struct kref *);
