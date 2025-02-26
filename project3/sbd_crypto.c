/*
 * A sample, extra-simple block driver. Updated for kernel 2.6.31.
 *
 * (C) 2003 Eklektix, Inc.
 * (C) 2010 Pat Patterson <pat at superpat dot com>
 * Redistributable under the terms of the GNU GPL.
 * 
 * Source: http://blog.superpat.com/2010/05/04/a-simple-block-driver-for-linux-kernel-2-6-31
 * Modifications: http://blog.superpat.com/2010/05/04/a-simple-block-driver-for-linux-kernel-2-6-31/comment-page-2/#comment-148884i
 *
 * References:
 * [1]
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>

#include <linux/crypto.h> /* transparent crypto functionality */

MODULE_LICENSE("Dual BSD/GPL");
/*
 * Commenting this out because its warning, and we don't use it.
 */
//static char *Version = "1.4";

static int major_num = 0;
module_param(major_num, int, 0);
static int logical_block_size = 512;
module_param(logical_block_size, int, 0);
static int nsectors = 1024; /* How big the drive is */
module_param(nsectors, int, 0);

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 512

/*
 * crypto key setup
 * I guess charp is a char pointer macro?
 */
static char* cryptoKey = "hunter42";
module_param(cryptoKey, charp, 0);

static char* cryptoAlg = "aes";
module_param(cryptoAlg, charp, 0);

/*
 * crypto cipher container
 */
static struct crypto_cipher *cipher;

/*
 * Our request queue.
 */
static struct request_queue *Queue;

/*
 * The internal representation of our device.
 */
static struct sbd_crypto_device {
	unsigned long size;
	spinlock_t lock;
	u8 *data;
	struct gendisk *gd;
} Device;

/*
 * Handle an I/O request.
 */
static void sbd_crypto_transfer(struct sbd_crypto_device *dev, sector_t sector,
	unsigned long nsect, char *buffer, int write) {
	unsigned long offset = sector * logical_block_size;
	unsigned long nbytes = nsect * logical_block_size;

        int cipherBlockSize = crypto_cipher_blocksize(cipher);

	if ((offset + nbytes) > dev->size) {
		printk (KERN_NOTICE "sbd_crypto: Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}
	if (write) {
                //no longer use this, as the crypto function does this
		//memcpy(dev->data + offset, buffer, nbytes);
                
                int i;
                for( i = 0; i < nbytes; i += cipherBlockSize ) {
                    crypto_cipher_encrypt_one( cipher, dev->data + offset + i, buffer + i );
                }
	} else {
                //no longer use this, as the crypto function does this
		//memcpy(buffer, dev->data + offset, nbytes);
                
                int i;
                for( i = 0; i < nbytes; i+= cipherBlockSize ) {
                    crypto_cipher_decrypt_one( cipher, buffer + i, dev->data + offset + i );
                }
        }
}

static void sbd_crypto_request(struct request_queue *q) {
	struct request *req;

	req = blk_fetch_request(q);
	while (req != NULL) {
		// blk_fs_request() was removed in 2.6.36 - many thanks to
		// Christian Paro for the heads up and fix...
		//if (!blk_fs_request(req)) {
		if (req == NULL || (req->cmd_type != REQ_TYPE_FS)) {
			printk (KERN_NOTICE "Skip non-CMD request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		sbd_crypto_transfer(&Device, blk_rq_pos(req), blk_rq_cur_sectors(req),
			bio_data(req->bio), rq_data_dir(req));
		if ( ! __blk_end_request_cur(req, 0) ) {
			req = blk_fetch_request(q);
		}
	}
}

/*
 * The HDIO_GETGEO ioctl is handled in blkdev_ioctl(), which
 * calls this. We need to implement getgeo, since we can't
 * use tools such as fdisk to partition the drive otherwise.
 */
int sbd_crypto_getgeo(struct block_device * block_device, struct hd_geometry * geo) {
	long size;

	/* We have no real geometry, of course, so make something up. */
	size = Device.size * (logical_block_size / KERNEL_SECTOR_SIZE);
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;
	return 0;
}

/*
 * The device operations structure.
 */
static struct block_device_operations sbd_crypto_ops = {
	.owner  = THIS_MODULE,
	.getgeo = sbd_crypto_getgeo
};

static int __init sbd_crypto_init(void) {
    	/*
         * setup crypto cipher based on module choice (default AES)
         * after cipher is prepared setup the crypto key
         */
        printk( "sbd_crypto: setting up crypto with key %s using cipher algorithm %s.\n", cryptoKey, cryptoAlg );
        cipher = crypto_alloc_cipher( cryptoAlg, 0, 0 );
        if ( !cipher ) {
            printk( KERN_WARNING "sbd_crypto: unable to initialize crypto with key %s and cipher algorithm %s!\n", cryptoKey, cryptoAlg );
            goto out;
        } else {
            crypto_cipher_setkey( cipher, cryptoKey, strlen(cryptoKey) );
            printk( "sbd_crypto: set up crypto successfully." );
        }

        /*
	 * Set up our internal device.
	 */
	Device.size = nsectors * logical_block_size;
	spin_lock_init(&Device.lock);
	Device.data = vmalloc(Device.size);
	if (Device.data == NULL)
		return -ENOMEM;
	/*
	 * Get a request queue.
	 */
	Queue = blk_init_queue(sbd_crypto_request, &Device.lock);
	if (Queue == NULL)
		goto out;
	blk_queue_logical_block_size(Queue, logical_block_size);
	/*
	 * Get registered.
	 */
	major_num = register_blkdev(major_num, "sbd_crypto");
	if (major_num < 0) {
		printk(KERN_WARNING "sbd_crypto: unable to get major number\n");
		goto out;
	}
        /*
	 * And the gendisk structure.
	 */
	Device.gd = alloc_disk(16);
	if (!Device.gd)
		goto out_unregister;
	Device.gd->major = major_num;
	Device.gd->first_minor = 0;
	Device.gd->fops = &sbd_crypto_ops;
	Device.gd->private_data = &Device;
	strcpy(Device.gd->disk_name, "sbd_crypto0");
	set_capacity(Device.gd, nsectors);
	Device.gd->queue = Queue;
	add_disk(Device.gd);

	return 0;

out_unregister:
	unregister_blkdev(major_num, "sbd_crypto");
out:
	vfree(Device.data);
	return -ENOMEM;
}

static void __exit sbd_crypto_exit(void)
{
        crypto_free_cipher(cipher);
	del_gendisk(Device.gd);
	put_disk(Device.gd);
	unregister_blkdev(major_num, "sbd_crypto");
	blk_cleanup_queue(Queue);
	vfree(Device.data);
}

module_init(sbd_crypto_init);
module_exit(sbd_crypto_exit);
