#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sctp.h"
#include "agent.h"
#include "dtls_srtp.h"
#include "sdp.h"
#include "config.h"
#include "rtp.h"
#include "rtcp_packet.h"
#include "buffer.h"
#include "ports.h"
#include "peer_connection.h"

#define STATE_CHANGED(pc, curr_state) if(pc->oniceconnectionstatechange && pc->state != curr_state) { pc->oniceconnectionstatechange(curr_state, pc->user_data); pc->state = curr_state; }

struct PeerConnection {

  PeerOptions options;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;

  Sdp local_sdp;
  Sdp remote_sdp;

  void (*onicecandidate)(char *sdp, void *user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void *user_data);
  void (*ontrack)(uint8_t *packet, size_t bytes, void *user_data);
  void (*on_connected)(void *userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void *user_data);

  void *user_data;

  uint8_t temp_buf[CONFIG_MTU];
  uint8_t agent_buf[CONFIG_MTU];
  int agent_ret;
  int b_offer_created;

  Buffer *audio_rb;
  Buffer *video_rb;
  Buffer *data_rb;

  RtpPacketizer audio_packetizer;
  RtpPacketizer video_packetizer;

};

static int peer_connection_send_rtp_packet(PeerConnection *pc, uint8_t *packet, int bytes) {

  dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, packet, &bytes);

  return agent_send(&pc->agent, packet, bytes);
}

static void peer_connection_on_rtp_packet(uint8_t *packet, size_t bytes, void *user_data) {

#if 0
  printf("size (%d): ", bytes);
  for (int i = 0; i < 20; i++)
    printf("%.2x ", packet[i]);
  printf("\n");
#endif
#if 0
  uint64_t sum = 0;
  for (int i = 0; i < bytes; i++) {
		sum += packet[i];
	}
  printf("size = %d, sum = %ld\n", bytes, sum);
#endif
  peer_connection_send_rtp_packet((PeerConnection *) user_data, packet, bytes);
}

static int peer_connection_dtls_srtp_recv(void *ctx, unsigned char *buf, size_t len) {

  static const int MAX_RECV = 10000;
  int recv_max = 0; 
  int ret; 
  DtlsSrtp *dtls_srtp = (DtlsSrtp *) ctx; 
  PeerConnection *pc = (PeerConnection *) dtls_srtp->user_data;

  if (pc->agent_ret > 0 && pc->agent_ret < len) {

    memcpy(buf, pc->agent_buf, pc->agent_ret);
    return pc->agent_ret;
  }

  while (recv_max < MAX_RECV) {

    ret = agent_recv(&pc->agent, buf, len);

    if (ret > 0) {
      break;
    }
    usleep(1*1000);
    recv_max++;

  }
  return ret;	
}

static int peer_connection_dtls_srtp_send(void *ctx, const uint8_t *buf, size_t len) {
  
  DtlsSrtp *dtls_srtp = (DtlsSrtp *) ctx; 
  PeerConnection *pc = (PeerConnection *) dtls_srtp->user_data;

  //LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len); 
  return agent_send(&pc->agent, buf, len);
  
}

static void peer_connection_incoming_rtcp(PeerConnection *pc, uint8_t *buf, size_t len) {

  RtcpHeader rtcp_header = {0};
  memcpy(&rtcp_header, buf, sizeof(rtcp_header));
  switch(rtcp_header.type) {
    case RTCP_RR:
      if(rtcp_header.rc > 0) {
        RtcpRr rtcp_rr = rtcp_packet_parse_rr(buf);
        uint32_t fraction = ntohl(rtcp_rr.report_block[0].flcnpl) >> 24;
        uint32_t total = ntohl(rtcp_rr.report_block[0].flcnpl) & 0x00FFFFFF;
        if(pc->on_receiver_packet_loss && fraction > 0) {
          pc->on_receiver_packet_loss((float)fraction/256.0, total, pc->user_data);
        }
      }
      break;
    default:
      break;
  }
}

PeerConnection* peer_connection_create(PeerOptions *options, void *user_data) {

  PeerConnection *pc = calloc(1, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }

  memcpy(&pc->options, options, sizeof(PeerOptions));

  uint32_t ssrc;
  RtpPayloadType type;

  pc->user_data = user_data;
  pc->agent.mode = AGENT_MODE_CONTROLLED;

  memset(&pc->sctp, 0, sizeof(pc->sctp));
  dtls_srtp_init(&pc->dtls_srtp, DTLS_SRTP_ROLE_SERVER, pc);

  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;

  if (pc->options.datachannel) {
    LOGI("Datachannel allocates heap size: %d", DATA_RB_DATA_LENGTH);
    pc->data_rb = buffer_new(DATA_RB_DATA_LENGTH);
  }

  if (pc->options.audio_codec) {
    LOGI("Audio allocates heap size: %d", AUDIO_RB_DATA_LENGTH);
    pc->audio_rb = buffer_new(AUDIO_RB_DATA_LENGTH);
    rtp_packetizer_init(&pc->audio_packetizer, pc->options.audio_codec,
     peer_connection_on_rtp_packet, (void*)pc);
  }

  if (pc->options.video_codec) {
    LOGI("Video allocates heap size: %d", VIDEO_RB_DATA_LENGTH);
    pc->video_rb = buffer_new(VIDEO_RB_DATA_LENGTH);
    rtp_packetizer_init(&pc->video_packetizer, pc->options.video_codec,
     peer_connection_on_rtp_packet, (void*)pc);
  }

  return pc;
}

void peer_connection_destroy(PeerConnection *pc) {

  if (pc) {

    free(pc);
    pc = NULL;
  }
}

int peer_connection_send_audio(PeerConnection *pc, const uint8_t *buf, size_t len) {

  if (pc->state != PEER_CONNECTION_CONNECTED) {
    //LOGE("dtls_srtp not connected");
    return -1;
  }

  return buffer_push_tail(pc->audio_rb, buf, len);
}

int peer_connection_send_video(PeerConnection *pc, const uint8_t *buf, size_t len) {

  if (pc->state != PEER_CONNECTION_CONNECTED) {
    //LOGE("dtls_srtp not connected");
    return -1;
  }

  return buffer_push_tail(pc->video_rb, buf, len);
}

int peer_connection_datachannel_send(PeerConnection *pc, char *message, size_t len) {

  if(!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }

  return buffer_push_tail(pc->data_rb, (const uint8_t*)message, len);
}

static void peer_connection_state_new(PeerConnection *pc) {

  int b_video = pc->options.video_codec != CODEC_NONE;
  int b_audio = pc->options.audio_codec != CODEC_NONE;
  int b_datachannel = pc->options.datachannel;
  char *description = (char*)pc->temp_buf;

  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));

  agent_reset(&pc->agent);

  dtls_srtp_reset_session(&pc->dtls_srtp);

  pc->sctp.connected = 0;

  agent_gather_candidates(&pc->agent);

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));

  memset(&pc->local_sdp, 0, sizeof(pc->local_sdp));
  // TODO: check if we have video or audio codecs
  sdp_create(&pc->local_sdp, b_video, b_audio, b_datachannel);

  if (pc->options.video_codec == CODEC_H264) {

    sdp_append_h264(&pc->local_sdp);
    sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
    sdp_append(&pc->local_sdp, "a=setup:actpass");
    strcat(pc->local_sdp.content, description);
  }

  if (pc->options.audio_codec == CODEC_PCMA) {

    sdp_append_pcma(&pc->local_sdp);
    sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
    sdp_append(&pc->local_sdp, "a=setup:actpass");
    strcat(pc->local_sdp.content, description);
  }

  if (pc->options.datachannel) {
    sdp_append_datachannel(&pc->local_sdp);
    sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
    sdp_append(&pc->local_sdp, "a=setup:actpass");
    strcat(pc->local_sdp.content, description);
  }

  pc->b_offer_created = 1;

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->local_sdp.content, pc->user_data);
  }
}

int peer_connection_loop(PeerConnection *pc) {

  memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
  pc->agent_ret = -1;

  switch (pc->state) {
    case PEER_CONNECTION_NEW:

      if (!pc->b_offer_created) {
        peer_connection_state_new(pc);
      }
      break;

    case PEER_CONNECTION_CHECKING:

      agent_select_candidate_pair(&pc->agent);

      if (agent_connectivity_check(&pc->agent)) {

        LOGD("Connectivity check success. pair: %p", pc->agent.nominated_pair);

        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
        pc->agent.selected_pair = pc->agent.nominated_pair;
      }

      agent_recv(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf));

      break;

    case PEER_CONNECTION_CONNECTED:
      if (pc->dtls_srtp.state == DTLS_SRTP_STATE_INIT) {

        if (dtls_srtp_handshake(&pc->dtls_srtp, NULL) == 0) {

          LOGD("DTLS-SRTP handshake done");

          if (pc->options.datachannel) {
            LOGI("SCTP create socket");
            sctp_create_socket(&pc->sctp, &pc->dtls_srtp);
          }

        }
      } else if (pc->dtls_srtp.state == DTLS_SRTP_STATE_CONNECTED) {

        int bytes;
        uint8_t *data = NULL;

        data = buffer_peak_head(pc->video_rb, &bytes);
        if (data) {
          rtp_packetizer_encode(&pc->video_packetizer, data, bytes);
          buffer_pop_head(pc->video_rb);
        }

        data = buffer_peak_head(pc->audio_rb, &bytes);
        if (data) {
          rtp_packetizer_encode(&pc->audio_packetizer, data, bytes);
          buffer_pop_head(pc->audio_rb);
        }

        data = buffer_peak_head(pc->data_rb, &bytes);
        if (data) {

           if (pc->options.datachannel == DATA_CHANNEL_STRING)
             sctp_outgoing_data(&pc->sctp, (char*)data, bytes, PPID_STRING);
           else
             sctp_outgoing_data(&pc->sctp, (char*)data, bytes, PPID_BINARY);
           buffer_pop_head(pc->data_rb);
        }

        if ((pc->agent_ret = agent_recv(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf))) > 0) {
          LOGD("agent_recv %d", pc->agent_ret);

          if (rtcp_packet_validate(pc->agent_buf, pc->agent_ret)) {
            LOGD("Got RTCP packet");
            dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
            peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);

          } else if (dtls_srtp_validate(pc->agent_buf)) {

            int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
            LOGD("Got DTLS data %d", ret);

            if (ret > 0) {
              sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
            }

          } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
            LOGD("Got RTP packet");

          }

        }
      }
      break;
    case PEER_CONNECTION_COMPLETED:
      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  return 0;
}

void peer_connection_set_remote_description(PeerConnection *pc, const char *sdp_text) {

  agent_set_remote_description(&pc->agent, (char*)sdp_text);
  STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
}

void peer_connection_create_offer(PeerConnection *pc) {

  STATE_CHANGED(pc, PEER_CONNECTION_NEW);
  pc->b_offer_created = 0;
}

int peer_connection_send_rtcp_pil(PeerConnection *pc, uint32_t ssrc) {

  int ret = -1;
  uint8_t plibuf[128];
  rtcp_packet_get_pli(plibuf, 12, ssrc);
 
  //TODO: encrypt rtcp packet
  //guint size = 12;
  //dtls_transport_encrypt_rctp_packet(pc->dtls_transport, plibuf, &size);
  //ret = nice_agent_send(pc->nice_agent, pc->stream_id, pc->component_id, size, (gchar*)plibuf);

  return ret;
}

// callbacks
void peer_connection_on_connected(PeerConnection *pc, void (*on_connected)(void *userdata)) {

  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection *pc,
 void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void *userdata)) {

  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_onicecandidate(PeerConnection *pc, void (*onicecandidate)(char *sdp_text, void *userdata)) {

  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection *pc,
 void (*oniceconnectionstatechange)(PeerConnectionState state, void *userdata)) {

  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ontrack(PeerConnection *pc, void (*ontrack)(uint8_t *packet, size_t byte, void *userdata)) {

  pc->ontrack = ontrack;
}

void peer_connection_ondatachannel(PeerConnection *pc,
 void (*onmessasge)(char *msg, size_t len, void *userdata),
 void (*onopen)(void *userdata),
 void (*onclose)(void *userdata)) {

  if (pc) {

    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessasge);
  }
}

