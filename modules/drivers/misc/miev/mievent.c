#define pr_fmt(fmt) "miev: " fmt

#include <miev/mievent.h>
#include <linux/slab.h>
#include <linux/ktime.h>

#define MIEVENT_MAX_SIZE 4096

struct misight_mievent *cdev_tevent_alloc(unsigned int eventid)
{
	struct misight_mievent *event;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return NULL;
	event->head = kzalloc(sizeof(*event->head), GFP_KERNEL);
	if (!event->head) {
		kfree(event);
		return NULL;
	}
	event->eventid = eventid;
	event->time = ktime_get_real_seconds();
	return event;
}
EXPORT_SYMBOL_GPL(cdev_tevent_alloc);

static int mievent_add(struct misight_mievent *event, const char *key,
		      const char *value, int type)
{
	struct mievent_payload *payload;
	size_t key_len, value_len;

	if (!event || !event->head || !key || !value)
		return -EINVAL;
	key_len = strnlen(key, MIEVENT_MAX_SIZE);
	value_len = strnlen(value, MIEVENT_MAX_SIZE);
	if (key_len == MIEVENT_MAX_SIZE || value_len == MIEVENT_MAX_SIZE)
		return -E2BIG;
	payload = kmalloc(sizeof(*payload) + key_len + value_len + 2, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	payload->key = (char *)(payload + 1);
	payload->value = payload->key + key_len + 1;
	memcpy(payload->key, key, key_len + 1);
	memcpy(payload->value, value, value_len + 1);
	payload->type = type;
	payload->next = event->head->next;
	event->head->next = payload;
	return 0;
}

int cdev_tevent_add_int(struct misight_mievent *event, const char *key, long value)
{
	char number[32];

	snprintf(number, sizeof(number), "%ld", value);
	return mievent_add(event, key, number, INT_T);
}
EXPORT_SYMBOL_GPL(cdev_tevent_add_int);

int cdev_tevent_add_str(struct misight_mievent *event, const char *key, const char *value)
{
	return mievent_add(event, key, value, STR_T);
}
EXPORT_SYMBOL_GPL(cdev_tevent_add_str);

static int mievent_put(char *buffer, size_t *used, char value)
{
	if (*used == MIEVENT_MAX_SIZE)
		return -EMSGSIZE;
	buffer[(*used)++] = value;
	return 0;
}

static int mievent_string(char *buffer, size_t *used, const char *value, bool quote)
{
	static const char hex[] = "0123456789abcdef";
	const unsigned char *p = (const unsigned char *)value;
	int ret;

	if (quote && mievent_put(buffer, used, '"'))
		return -EMSGSIZE;
	for (; *p; p++) {
		if (quote && *p < 0x20) {
			const char escape[] = { '\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15] };

			for (size_t i = 0; i < sizeof(escape); i++)
				if (mievent_put(buffer, used, escape[i]))
					return -EMSGSIZE;
		} else {
			if (quote && (*p == '"' || *p == '\\') && mievent_put(buffer, used, '\\'))
				return -EMSGSIZE;
			ret = mievent_put(buffer, used, *p);
			if (ret)
				return ret;
		}
	}
	return quote ? mievent_put(buffer, used, '"') : 0;
}

static void free_list(struct mievent_payload *head)
{
	struct mievent_payload *payload, *next;

	for (payload = head->next; payload; payload = next) {
		next = payload->next;
		kfree(payload);
	}
	head->next = NULL;
}

int cdev_tevent_write(struct misight_mievent *event)
{
	struct mievent_payload *payload;
	char *buffer;
	size_t used;
	int ret;

	if (!event || !event->head)
		return -EINVAL;
	buffer = kmalloc(MIEVENT_MAX_SIZE + 1, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	used = scnprintf(buffer, MIEVENT_MAX_SIZE + 1, "EventId %u -t %lld -paraList {",
			 event->eventid, event->time);
	for (payload = event->head->next; payload; payload = payload->next) {
		ret = mievent_string(buffer, &used, payload->key, true);
		if (ret)
			goto out;
		ret = mievent_put(buffer, &used, ':');
		if (ret)
			goto out;
		ret = mievent_string(buffer, &used, payload->value, payload->type == STR_T);
		if (ret)
			goto out;
		if (payload->next && mievent_put(buffer, &used, ',')) {
			ret = -EMSGSIZE;
			goto out;
		}
	}
	ret = mievent_put(buffer, &used, '}');
	if (ret)
		goto out;
	buffer[used] = '\0';
	ret = write_kbuf(buffer, used);
	if (ret == used) {
		free_list(event->head);
		ret = 0;
	} else if (ret >= 0) {
		ret = -EIO;
	}
out:
	kfree(buffer);
	return ret;
}
EXPORT_SYMBOL_GPL(cdev_tevent_write);

void cdev_tevent_destroy(struct misight_mievent *event)
{
	if (!event)
		return;
	free_list(event->head);
	kfree(event->head);
	kfree(event);
}
EXPORT_SYMBOL_GPL(cdev_tevent_destroy);
