#include "network.h"

AVPacket *network_packet_alloc(void)
{
    return av_packet_alloc();
}

void network_packet_free(AVPacket **packet)
{
    av_packet_free(packet);
}

void network_packet_unref(AVPacket *packet)
{
    av_packet_unref(packet);
}

int network_packet_new(AVPacket *packet, int size)
{
    return av_new_packet(packet, size);
}

void network_packet_move_ref(AVPacket *dst, AVPacket *src)
{
    av_packet_move_ref(dst, src);
}
