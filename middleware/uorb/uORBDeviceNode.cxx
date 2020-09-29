
#include <nuttx/config.h>
#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <cstdio>

#include <arch/irq.h>

#include "uORBDeviceNode.h"

#define ATOMIC_ENTER irqstate_t flags = enter_critical_section()
#define ATOMIC_LEAVE leave_critical_section(flags)


uORBDeviceNode::uORBDeviceNode(const struct orb_metadata *meta, const uint8_t instance, const char *path,
			     uint8_t queue_size) :
	//CDev(path),
	_meta(meta),
	_instance(instance),
	_queue_size(queue_size)
{
}

uORBDeviceNode::~uORBDeviceNode()
{
	delete[] _data;

	//CDev::unregister_driver_and_memory();
}

ssize_t uORBDeviceNode::publish(const orb_metadata *meta, orb_advert_t handle, const void *data)
{
	uORBDeviceNode *devnode = (uORBDeviceNode *)handle;
	int ret;

	/* check if the device handle is initialized and data is valid */
	if ((devnode == nullptr) || (meta == nullptr) || (data == nullptr)) {
		errno = EFAULT;
		return -1;
	}

	/* check if the orb meta data matches the publication */
	if (devnode->_meta != meta) {
		errno = EINVAL;
		return -1;
	}

	/* call the devnode write method with no file pointer */
	ret = devnode->write(nullptr, (const char *)data, meta->o_size);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	if (ret != (int)meta->o_size) {
		errno = EIO;
		return -1;
	}

#ifdef ORB_COMMUNICATOR
	/*
	 * if the write is successful, send the data over the Multi-ORB link
	 */
	uORBCommunicator::IChannel *ch = uORB::Manager::get_instance()->get_uorb_communicator();

	if (ch != nullptr) {
		if (ch->send_message(meta->o_name, meta->o_size, (uint8_t *)data) != 0) {
			PX4_ERR("Error Sending [%s] topic data over comm_channel", meta->o_name);
			return PX4_ERROR;
		}
	}

#endif /* ORB_COMMUNICATOR */

	return 0;
}

int uORBDeviceNode::unadvertise(orb_advert_t handle)
{
	if (handle == nullptr) {
		return -EINVAL;
	}

	uORBDeviceNode *devnode = (uORBDeviceNode *)handle;

	/*
	 * We are cheating a bit here. First, with the current implementation, we can only
	 * have multiple publishers for instance 0. In this case the caller will have
	 * instance=nullptr and _published has no effect at all. Thus no unadvertise is
	 * necessary.
	 * In case of multiple instances, we have at most 1 publisher per instance and
	 * we can signal an instance as 'free' by setting _published to false.
	 * We never really free the DeviceNode, for this we would need reference counting
	 * of subscribers and publishers. But we also do not have a leak since future
	 * publishers reuse the same DeviceNode object.
	 */
	devnode->_advertised = false;

	return 0;
}


void uORBDeviceNode::add_internal_subscriber()
{
	lock();
	  _subscriber_count++;

#ifdef ORB_COMMUNICATOR
	uORBCommunicator::IChannel *ch = uORB::Manager::get_instance()->get_uorb_communicator();

	if (ch != nullptr && _subscriber_count > 0) {
		unlock(); //make sure we cannot deadlock if add_subscription calls back into DeviceNode
		ch->add_subscription(_meta->o_name, 1);

	} else
#endif /* ORB_COMMUNICATOR */

	{
		unlock();
	}
}

void uORBDeviceNode::remove_internal_subscriber()
{
	lock();
	  _subscriber_count--;

#ifdef ORB_COMMUNICATOR
	uORBCommunicator::IChannel *ch = uORB::Manager::get_instance()->get_uorb_communicator();

	if (ch != nullptr && _subscriber_count == 0) {
		unlock(); //make sure we cannot deadlock if remove_subscription calls back into DeviceNode
		ch->remove_subscription(_meta->o_name);

	} else
#endif /* ORB_COMMUNICATOR */
	{
		unlock();
	}
}

bool uORBDeviceNode::copy(void *dst, unsigned &generation)
{
	if ((dst != nullptr) && (_data != nullptr)) {
		if (_queue_size == 1) {
			ATOMIC_ENTER;
			memcpy(dst, _data, _meta->o_size);
			generation = _generation.load();
			ATOMIC_LEAVE;
			return true;

		} else {
			ATOMIC_ENTER;
			const unsigned current_generation = _generation.load();

			if (current_generation > generation + _queue_size) {
				// Reader is too far behind: some messages are lost
				generation = current_generation - _queue_size;
			}

			if ((current_generation == generation) && (generation > 0)) {
				/* The subscriber already read the latest message, but nothing new was published yet.
				* Return the previous message
				*/
				--generation;
			}

			memcpy(dst, _data + (_meta->o_size * (generation % _queue_size)), _meta->o_size);
			ATOMIC_LEAVE;

			if (generation < current_generation) {
				++generation;
			}

			return true;
		}
	}

	return false;
}


int uORBDeviceNode::update_queue_size(unsigned int queue_size)
{
	if (_queue_size == queue_size) {
		return 0;
	}

	//queue size is limited to 255 for the single reason that we use uint8 to store it
	if (_data || _queue_size > queue_size || queue_size > 255) {
		return -1;
	}

	_queue_size = queue_size;
	return 0;
}

bool uORBDeviceNode::print_statistics(int max_topic_length)
{
	if (!_advertised) {
		return false;
	}

	lock();

	const uint8_t instance = get_instance();
	const int8_t sub_count = subscriber_count();
	const uint8_t queue_size = get_queue_size();

	unlock();

	//PX4_INFO_RAW("%-*s %2i %4i %2i %4i %s\n", max_topic_length, get_meta()->o_name, (int)instance, (int)sub_count, queue_size, get_meta()->o_size, get_devname());

	return true;
}


bool uORBDeviceNode::register_callback(uORB::SubscriptionCallback *callback_sub)
{
	if (callback_sub != nullptr) {
		ATOMIC_ENTER;

		// prevent duplicate registrations
		for (auto existing_callbacks : _callbacks) {
			if (callback_sub == existing_callbacks) {
				ATOMIC_LEAVE;
				return true;
			}
		}

		_callbacks.add(callback_sub);
		ATOMIC_LEAVE;
		return true;
	}

	return false;
}
 
void uORBDeviceNode::unregister_callback(uORB::SubscriptionCallback *callback_sub)
{
	ATOMIC_ENTER;
	_callbacks.remove(callback_sub);
	ATOMIC_LEAVE;
}



int uORBDeviceNode::open(FAR struct file *filep)
{
	/* is this a publisher? */
	if (filp->f_oflags == O_WRONLY) {

		lock();
		mark_as_advertised();
		unlock();

		/* now complete the open */
		//return CDev::open(filp);
	}

	/* is this a new subscriber? */
	if (filp->f_oflags == O_RDONLY) {

		/* allocate subscriber data */
		SubscriptionInterval *sd = new SubscriptionInterval(_meta, 0, _instance);

		if (nullptr == sd) {
			return -ENOMEM;
		}

		filp->f_priv = (void *)sd;

		// int ret = CDev::open(filp);

		// if (ret != 0) {
		// 	printf"Error. CDev::open failed"()
		// 	delete sd;
		// }

		return 0;
	}

	if (filp->f_oflags == 0) {
		//return CDev::open(filp);
	}

	/* can only be pub or sub, not both */
	return -EINVAL;
}

int uORBDeviceNode::close(FAR struct file *filep)
{
	if (filp->f_oflags == O_RDONLY) { /* subscriber */
		SubscriptionInterval *sd = filp_to_subscription(filp);
		delete sd;
	}

	//return CDev::close(filp);
}

ssize_t uORBDeviceNode::read(FAR struct file *filep, FAR char *buffer, size_t buflen)
{
	/* if the caller's buffer is the wrong size, that's an error */
	if (buflen != _meta->o_size) {
		return -EIO;
	}

	return filp_to_subscription(filp)->copy(buffer) ? _meta->o_size : 0;
}

ssize_t uORBDeviceNode::write(FAR struct file *filep, FAR const char *buffer, size_t buflen)
{
	/*
	 * Writes are legal from interrupt context as long as the
	 * object has already been initialised from thread context.
	 *
	 * Writes outside interrupt context will allocate the object
	 * if it has not yet been allocated.
	 *
	 * Note that filp will usually be NULL.
	 */
	if (nullptr == _data) {

		if (!up_interrupt_context()) {

			lock();

			/* re-check size */
			if (nullptr == _data) {
				_data = new uint8_t[_meta->o_size * _queue_size];
			}
			unlock();
		}

		/* failed or could not allocate */
		if (nullptr == _data) {
			return -ENOMEM;
		}
	}

	/* If write size does not match, that is an error */
	if (_meta->o_size != buflen) {
		return -EIO;
	}

	/* Perform an atomic copy. */
	ATOMIC_ENTER;
	/* wrap-around happens after ~49 days, assuming a publisher rate of 1 kHz */
	unsigned generation = _generation.fetch_add(1);

	memcpy(_data + (_meta->o_size * (generation % _queue_size)), buffer, _meta->o_size);

	// callbacks
	for (auto item : _callbacks) {
		item->call();
	}

	ATOMIC_LEAVE;

	/* notify any poll waiters */
	poll_notify(POLLIN);

	return _meta->o_size;
}

int uORBDeviceNode::ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
	switch (cmd) {
	case ORBIOCUPDATED: {
			ATOMIC_ENTER;
			*(bool *)arg = filp_to_subscription(filp)->updated();
			ATOMIC_LEAVE;
			return 0;
		}

	case ORBIOCSETINTERVAL:
		filp_to_subscription(filp)->set_interval_us(arg);
		return 0;

	case ORBIOCGADVERTISER:
		*(uintptr_t *)arg = (uintptr_t)this;
		return 0;

	case ORBIOCSETQUEUESIZE: {
			lock();
			int ret = update_queue_size(arg);
			unlock();
			return ret;
		}

	case ORBIOCGETINTERVAL:
		*(unsigned *)arg = filp_to_subscription(filp)->get_interval_us();
		return 0;

	case ORBIOCISADVERTISED:
		*(unsigned long *)arg = _advertised;

		return 0;

	default:
		/* give it to the superclass */
		//return CDev::ioctl(filp, cmd, arg);
		return -ENOTTY;
	}
}






static int uORBDeviceNode_open(FAR struct file *filep)
{
	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);

	return uORBDev->open(filep);
}

static int uORBDeviceNode_close(FAR struct file *filep)
{
	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);

	return uORBDev->close(filep);
}

static ssize_t uORBDeviceNode_read(FAR struct file *filep, FAR char *buffer, size_t buflen)
{
	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);

	return uORBDev->read(filep, buffer, buflen);
}

static ssize_t uORBDeviceNode_write(FAR struct file *filep, FAR const char *buffer, size_t buflen)
{
	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);

	return uORBDev->write(filep, buffer, buflen);
}

// static off_t uORBDeviceNode_seek(FAR struct file *filep, off_t offset, int whence)
// {
// 	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);
// 	return uORBDev->seek(filep, offset, whence);
// }

static int uORBDeviceNode_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);

	return uORBDev->ioctl(filep, cmd, arg);
}

// static int uORBDeviceNode_poll(FAR struct file *filep, struct pollfd *fds, bool setup)
// {
// 	uORBDeviceNode *uORBDev = (uORBDeviceNode *)(filep->f_inode->i_private);

// 	return uORBDev->poll(filep, fds, setup);
// }


static const struct file_operations uorb_fops =
{
  uORBDeviceNode_open,  /* open */
  uORBDeviceNode_close, /* close */
  uORBDeviceNode_read,  /* read */
  uORBDeviceNode_write, /* write */
  NULL,         /* seek */
  uORBDeviceNode_ioctl,         /* ioctl */
  NULL          /* poll */
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , NULL        /* unlink */
#endif
};

int uORBNodeDevice_register(FAR const char *path)
{
  printf("registered uORBDeviceNode\n");
  FAR uORBDeviceNode *uORBDev;

  /* Allocate the upper-half data structure */

  uORBDev = (FAR uORBDeviceNode *)kmm_zalloc(sizeof(uORBDeviceNode));

  if (!uORBDev)
    {
      lcderr("ERROR: Allocation failed\n");
      return -ENOMEM;
    }


  lcdinfo("Registering %s\n", path);
  return register_driver(path, &uorb_fops, 0666, uORBDev);
}

