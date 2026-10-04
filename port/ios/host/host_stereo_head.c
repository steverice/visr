/* The head's angles for head-tracked stereo (host_stereo_head.h). */
#include "host_stereo_head.h"
#include <math.h>

/* angle wrapped into -pi..pi */
static float wrapped(float angle)
{
	while (angle > (float)M_PI)
		angle -= 2.0f * (float)M_PI;
	while (angle < -(float)M_PI)
		angle += 2.0f * (float)M_PI;
	return angle;
}

void host_stereo_head_turn(struct host_stereo_head *head, const float right[3], const float up[3],
	const float back[3], struct halo_stereo_frame *frame)
{
	float forward[3] = { -back[0], -back[1], -back[2] };
	/* yaw about the room's up, left positive, as the game's yaw; pitch up
	positive; roll left ear down positive */
	float yaw = atan2f(-forward[0], -forward[2]);
	float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[1])));
	float roll = atan2f(right[1], up[1]);

	/* the yaw is a turn since the last frame (the body's yaw is the game's,
	which the stick turns too); the pitch and roll are the head's own, so the
	camera's up stays the room's: a pitch kept as turns drifts from the
	head's (the game starts level, levels itself, clamps), and the yaw about
	the room's up then rolls the world as the head pans */
	if (head->known)
		frame->head_yaw = wrapped(yaw - head->yaw);
	frame->head_pitch = pitch;
	frame->head_roll = roll;
	head->yaw = yaw;
	head->pitch = pitch;
	head->roll = roll;
	head->known = 1;
}

void host_stereo_head_hold(const struct host_stereo_head *head, struct halo_stereo_frame *frame)
{
	frame->head_pitch = head->known ? head->pitch : 0.0f;
	frame->head_roll = head->known ? head->roll : 0.0f;
}
