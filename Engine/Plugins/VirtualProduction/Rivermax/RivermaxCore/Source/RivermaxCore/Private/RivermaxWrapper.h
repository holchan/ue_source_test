// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "RivermaxHeader.h"


namespace UE::RivermaxCore::Private
{
	typedef __export const rmx_version * (*PFNRMX_GET_VERSION_NUMBERS)(void);
	typedef __export const char * (*PFNRMX_GET_VERSION_STRING)(void);
	typedef __export rmx_status (*PFNRMX_SET_CPU_AFFINITY)(const uint64_t * bitmask,size_t core_count);
	typedef __export rmx_status (*PFNRMX_ENABLE_SYSTEM_SIGNAL_HANDLING)(void);
	typedef __export rmx_status (*PFN_RMX_INIT)(const rmx_version * policy);
	typedef __export rmx_status (*PFNRMX_CLEANUP)(void);
	typedef __export size_t (*PFNRMX_GET_DEVICE_LIST)(rmx_device_list * * list);
	typedef __export void (*PFNRMX_FREE_DEVICE_LIST)(rmx_device_list * list);
	typedef __export size_t (*PFNRMX_GET_DEVICE_COUNT)(const rmx_device_list * list);
	typedef __export const rmx_device * (*PFNRMX_GET_DEVICE)(const rmx_device_list * list,size_t index);
	typedef __export const char * (*PFNRMX_GET_DEVICE_INTERFACE_NAME)(const rmx_device * device);
	typedef __export size_t (*PFNRMX_GET_DEVICE_IP_COUNT)(const rmx_device * device);
	typedef __export const rmx_ip_addr * (*PFNRMX_GET_DEVICE_IP_ADDRESS)(const rmx_device * device,size_t index);
	typedef __export const uint8_t * (*PFNRMX_GET_DEVICE_MAC_ADDRESS)(const rmx_device * device);
	typedef __export uint32_t (*PFNRMX_GET_DEVICE_ID)(const rmx_device * device);
	typedef __export const char * (*PFNRMX_GET_DEVICE_SERIAL_NUMBER)(const rmx_device * device);
	typedef __export rmx_status (*PFNRMX_RETRIEVE_DEVICE_IFACE)(rmx_device_iface * device_iface,const rmx_ip_addr * ip);
	typedef __export rmx_status (*PFNRMX_ENQUIRE_DEVICE_CAPABILITIES)(const rmx_device_iface * device_iface,rmx_device_capabilities * caps);
	typedef __export rmx_status (*PFNRMX_APPLY_DEVICE_CONFIG)(const rmx_device_iface * device_iface,rmx_device_config * config);
	typedef __export rmx_status (*PFNRMX_REVERT_DEVICE_CONFIG)(const rmx_device_iface * device_iface,rmx_device_config * config);
	typedef __export void (*PFNRMX_INIT_USER_CLOCK)(rmx_user_clock_params * params);
	typedef __export void (*PFNRMX_SET_USER_CLOCK_HANDLER)(rmx_user_clock_params * params,rmx_user_clock_handler handler);
	typedef __export void (*PFNRMX_SET_USER_CLOCK_CONTEXT)(rmx_user_clock_params * params,void * ctx);
	typedef __export rmx_status (*PFNRMX_USE_USER_CLOCK)(const rmx_user_clock_params * params);
	typedef __export void (*PFNRMX_INIT_PTP_CLOCK)(rmx_ptp_clock_params * params);
	typedef __export void (*PFNRMX_SET_PTP_CLOCK_DEVICE)(rmx_ptp_clock_params * params,const rmx_device_iface * device_iface);
	typedef __export rmx_status (*PFNRMX_USE_PTP_CLOCK)(const rmx_ptp_clock_params * params);
	typedef __export rmx_status (*PFNRMX_GET_TIME)(rmx_time_type type,uint64_t * time);
	typedef __export void (*PFNRMX_INIT_EVENT_CHANNEL)(rmx_event_channel_params * params,rmx_stream_id id);
	typedef __export void (*PFNRMX_SET_EVENT_CHANNEL_HANDLE)(rmx_event_channel_params * params,rmx_event_channel_handle * handle);
	typedef __export rmx_status (*PFNRMX_ESTABLISH_EVENT_CHANNEL)(rmx_event_channel_params * params);
	typedef __export void (*PFNRMX_INIT_NOTIFICATION)(rmx_notification_params * params,rmx_stream_id id);
	typedef __export void (*PFNRMX_SET_NOTIFICATION_OVERLAPPED)(rmx_notification_params * params,OVERLAPPED * overlapped);
	typedef __export rmx_status (*PFNRMX_REQUEST_NOTIFICATION)(rmx_notification_params * params);
	typedef __export void (*PFNRMX_INIT_MEM_REGISTRY)(rmx_mem_reg_params * params,const rmx_device_iface * device_iface);
	typedef __export void (*PFNRMX_SET_MEM_REGISTRY_OPTION)(rmx_mem_reg_params * params,rmx_mem_reg_params_option option);
	typedef __export rmx_status (*PFNRMX_REGISTER_MEMORY)(rmx_mem_region * mem,const rmx_mem_reg_params * params);
	typedef __export rmx_status (*PFNRMX_DEREGISTER_MEMORY)(const rmx_mem_region * mem,const rmx_device_iface * device_iface);
	typedef __export void (*PFNRMX_INPUT_INIT_STREAM)(rmx_input_stream_params * params,rmx_input_stream_params_type type);
	typedef __export void (*PFNRMX_INPUT_SET_MEM_CAPACITY_IN_PACKETS)(rmx_input_stream_params * params,size_t count);
	typedef __export void (*PFNRMX_INPUT_SET_STREAM_NIC_ADDRESS)(rmx_input_stream_params * params,const struct sockaddr * nic_address);
	typedef __export void (*PFNRMX_INPUT_ENABLE_STREAM_OPTION)(rmx_input_stream_params * params,rmx_input_option option);
	typedef __export void (*PFNRMX_INPUT_SET_MEM_SUB_BLOCK_COUNT)(rmx_input_stream_params * params,size_t count);
	typedef __export void (*PFNRMX_INPUT_SET_ENTRY_SIZE_RANGE)(rmx_input_stream_params * params,size_t sub_block_id,size_t min_size,size_t max_size);
	typedef __export rmx_status (*PFNRMX_INPUT_DETERMINE_MEM_LAYOUT)(rmx_input_stream_params * params);
	typedef __export size_t (*PFNRMX_INPUT_GET_MEM_CAPACITY_IN_PACKETS)(const rmx_input_stream_params * params);
	typedef __export rmx_mem_region * (*PFNRMX_INPUT_GET_MEM_BLOCK_BUFFER)(rmx_input_stream_params * params,size_t sub_block_id);
	typedef __export size_t (*PFNRMX_INPUT_GET_STRIDE_SIZE)(const rmx_input_stream_params * params,size_t sub_block_id);
	typedef __export void (*PFNRMX_INPUT_SET_TIMESTAMP_FORMAT)(rmx_input_stream_params * params,rmx_input_timestamp_format format);
	typedef __export rmx_status (*PFNRMX_INPUT_CREATE_STREAM)(rmx_input_stream_params * params,rmx_stream_id * id);
	typedef __export rmx_status (*PFNRMX_INPUT_DESTROY_STREAM)(rmx_stream_id id);
	typedef __export void (*PFNRMX_INPUT_INIT_FLOW)(rmx_input_flow * flow);
	typedef __export void (*PFNRMX_INPUT_SET_FLOW_LOCAL_ADDR)(rmx_input_flow * flow,const struct sockaddr * local);
	typedef __export void (*PFNRMX_INPUT_SET_FLOW_REMOTE_ADDR)(rmx_input_flow * flow,const struct sockaddr * remote);
	typedef __export void (*PFNRMX_INPUT_SET_FLOW_TAG)(rmx_input_flow * flow,uint32_t tag);
	typedef __export rmx_status (*PFNRMX_INPUT_ATTACH_FLOW)(rmx_stream_id id,const rmx_input_flow * flow);
	typedef __export rmx_status (*PFNRMX_INPUT_DETACH_FLOW)(rmx_stream_id id,const rmx_input_flow * flow);
	typedef __export rmx_status (*PFNRMX_INPUT_SET_COMPLETION_MODERATION)(rmx_stream_id id,size_t min_count,size_t max_count,int timeout_usec);
	typedef __export void (*PFNRMX_INPUT_INIT_CHUNK_HANDLE)(rmx_input_chunk_handle * handle,rmx_stream_id id);
	typedef __export rmx_status (*PFNRMX_INPUT_GET_NEXT_CHUNK)(rmx_input_chunk_handle * handle);
	typedef __export const rmx_input_completion * (*PFNRMX_INPUT_GET_CHUNK_COMPLETION)(rmx_input_chunk_handle * handle);
	typedef __export const rmx_input_packet_info * (*PFNRMX_INPUT_GET_PACKET_INFO)(rmx_input_chunk_handle * handle,size_t packet_id);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_INIT)(rmx_output_media_stream_params * params);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_SDP)(rmx_output_media_stream_params * params,const char * sdp);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_IDX_IN_SDP)(rmx_output_media_stream_params * params,size_t media_block_idx);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_PACKETS_PER_FRAME)(rmx_output_media_stream_params * params,size_t count);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_SOURCE_PORTS)(rmx_output_media_stream_params * params,const uint16_t * ports,size_t count);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_PCP)(rmx_output_media_stream_params * params,uint8_t pcp);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_DSCP)(rmx_output_media_stream_params * params,uint8_t dscp);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_ECN)(rmx_output_media_stream_params * params,uint8_t ecn);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_INIT_MEM_BLOCKS)(rmx_output_media_mem_block * mem_blocks,size_t count);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_CHUNK_COUNT)(rmx_output_media_mem_block * mem_block,size_t count);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_SUB_BLOCK_COUNT)(rmx_output_media_mem_block * mem_block,size_t count);
	typedef __export rmx_mem_region * (*PFNRMX_OUTPUT_MEDIA_GET_SUB_BLOCK)(rmx_output_media_mem_block * mem_block,size_t sub_block_id);
	typedef __export rmx_mem_multi_key_region * (*PFNRMX_OUTPUT_MEDIA_GET_DUP_SUB_BLOCK)(rmx_output_media_mem_block * mem_block,size_t sub_block_id);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_PACKET_LAYOUT)(rmx_output_media_mem_block * mem_block,size_t sub_block_id,const uint16_t * packet_sizes);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_ASSIGN_MEM_BLOCKS)(rmx_output_media_stream_params * params,rmx_output_media_mem_block * mem_blocks,size_t count);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_PACKETS_PER_CHUNK)(rmx_output_media_stream_params * params,size_t packets_per_chunk);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_STRIDE_SIZE)(rmx_output_media_stream_params * params,size_t sub_block_id,const size_t stride_size);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_CREATE_STREAM)(rmx_output_media_stream_params * params,rmx_stream_id * id);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_DESTROY_STREAM)(rmx_stream_id id);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_INIT_CONTEXT)(rmx_output_media_context * context,rmx_stream_id id);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_CONTEXT_BLOCK)(rmx_output_media_context * context,size_t id);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_GET_LOCAL_ADDRESS)(const rmx_output_media_context * context,struct sockaddr * address);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_GET_REMOTE_ADDRESS)(const rmx_output_media_context * context,struct sockaddr * address);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_INIT_CHUNK_HANDLE)(rmx_output_media_chunk_handle * handle,rmx_stream_id id);
	typedef __export void (*PFNRMX_OUTPUT_MEDIA_SET_CHUNK_PACKET_COUNT)(rmx_output_media_chunk_handle * handle,size_t packets_in_chunk);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_GET_NEXT_CHUNK)(rmx_output_media_chunk_handle * handle);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_COMMIT_CHUNK)(const rmx_output_media_chunk_handle * handle,uint64_t time);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_CANCEL_UNSENT_CHUNKS)(const rmx_output_media_chunk_handle * handle);
	typedef __export rmx_status (*PFNRMX_OUTPUT_MEDIA_SKIP_CHUNKS)(const rmx_output_media_chunk_handle * handle,size_t chunks_count);
	typedef __export void (*PFNRMX_OUTPUT_GEN_INIT_STREAM)(rmx_output_gen_stream_params * params);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_PACKETS_PER_CHUNK)(rmx_output_gen_stream_params * params,size_t max_packet_count);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_LOCAL_ADDR)(rmx_output_gen_stream_params * params,const struct sockaddr * addr);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_REMOTE_ADDR)(rmx_output_gen_stream_params * params,const struct sockaddr * addr);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_MAX_SUB_BLOCKS)(rmx_output_gen_stream_params * params,size_t sub_blocks_per_packet);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_PCP)(rmx_output_gen_stream_params * params,uint8_t pcp);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_DSCP)(rmx_output_gen_stream_params * params,uint8_t dscp);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_ECN)(rmx_output_gen_stream_params * params,uint8_t ecn);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_RATE)(rmx_output_gen_stream_params * params,const rmx_output_gen_rate * rate);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GEN_CREATE_STREAM)(rmx_output_gen_stream_params * params,rmx_stream_id * id);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GEN_DESTROY_STREAM)(rmx_stream_id id);
	typedef __export void (*PFNRMX_OUTPUT_GEN_INIT_RATE)(rmx_output_gen_rate * rate,uint64_t bps);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_RATE_MAX_BURST)(rmx_output_gen_rate * rate,size_t packet_count);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_RATE_TYPICAL_PACKET_SIZE)(rmx_output_gen_rate * rate,size_t size_in_bytes);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GEN_UPDATE_RATE)(rmx_stream_id id,const rmx_output_gen_rate * rate);
	typedef __export void (*PFNRMX_OUTPUT_GEN_INIT_CHUNK_HANDLE)(rmx_output_gen_chunk_handle * handle,rmx_stream_id id);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GEN_GET_NEXT_CHUNK)(rmx_output_gen_chunk_handle * handle);
	typedef __export void (*PFNRMX_OUTPUT_GEN_SET_CHUNK_REMOTE_ADDR)(rmx_output_gen_chunk_handle * handle,const struct sockaddr * addr);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GEN_APPEND_PACKET_TO_CHUNK)(rmx_output_gen_chunk_handle * handle,const rmx_mem_region * sub_blocks,size_t count);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GEN_COMMIT_CHUNK)(rmx_output_gen_chunk_handle * handle,uint64_t time);
	typedef __export rmx_status (*PFNRMX_OUTPUT_GET_CHUNK_COUNT)(rmx_stream_id id,size_t * count);
	typedef __export rmx_status (*PFNRMX_OUTPUT_UPDATE_DSCP)(rmx_stream_id id,uint8_t dscp);
	typedef __export rmx_status (*PFNRMX_OUTPUT_UPDATE_ECN)(rmx_stream_id id,uint8_t ecn);
	

	struct RIVERMAX_API_FUNCTION_LIST
	{
		PFNRMX_GET_VERSION_NUMBERS rmx_get_version_numbers;
		PFNRMX_GET_VERSION_STRING rmx_get_version_string;
		PFNRMX_SET_CPU_AFFINITY rmx_set_cpu_affinity;
		PFNRMX_ENABLE_SYSTEM_SIGNAL_HANDLING rmx_enable_system_signal_handling;
		PFN_RMX_INIT _rmx_init;
		PFNRMX_CLEANUP rmx_cleanup;
		PFNRMX_GET_DEVICE_LIST rmx_get_device_list;
		PFNRMX_FREE_DEVICE_LIST rmx_free_device_list;
		PFNRMX_GET_DEVICE_COUNT rmx_get_device_count;
		PFNRMX_GET_DEVICE rmx_get_device;
		PFNRMX_GET_DEVICE_INTERFACE_NAME rmx_get_device_interface_name;
		PFNRMX_GET_DEVICE_IP_COUNT rmx_get_device_ip_count;
		PFNRMX_GET_DEVICE_IP_ADDRESS rmx_get_device_ip_address;
		PFNRMX_GET_DEVICE_MAC_ADDRESS rmx_get_device_mac_address;
		PFNRMX_GET_DEVICE_ID rmx_get_device_id;
		PFNRMX_GET_DEVICE_SERIAL_NUMBER rmx_get_device_serial_number;
		PFNRMX_RETRIEVE_DEVICE_IFACE rmx_retrieve_device_iface;
		PFNRMX_ENQUIRE_DEVICE_CAPABILITIES rmx_enquire_device_capabilities;
		PFNRMX_APPLY_DEVICE_CONFIG rmx_apply_device_config;
		PFNRMX_REVERT_DEVICE_CONFIG rmx_revert_device_config;
		PFNRMX_INIT_USER_CLOCK rmx_init_user_clock;
		PFNRMX_SET_USER_CLOCK_HANDLER rmx_set_user_clock_handler;
		PFNRMX_SET_USER_CLOCK_CONTEXT rmx_set_user_clock_context;
		PFNRMX_USE_USER_CLOCK rmx_use_user_clock;
		PFNRMX_INIT_PTP_CLOCK rmx_init_ptp_clock;
		PFNRMX_SET_PTP_CLOCK_DEVICE rmx_set_ptp_clock_device;
		PFNRMX_USE_PTP_CLOCK rmx_use_ptp_clock;
		PFNRMX_GET_TIME rmx_get_time;
		PFNRMX_INIT_EVENT_CHANNEL rmx_init_event_channel;
		PFNRMX_SET_EVENT_CHANNEL_HANDLE rmx_set_event_channel_handle;
		PFNRMX_ESTABLISH_EVENT_CHANNEL rmx_establish_event_channel;
		PFNRMX_INIT_NOTIFICATION rmx_init_notification;
		PFNRMX_SET_NOTIFICATION_OVERLAPPED rmx_set_notification_overlapped;
		PFNRMX_REQUEST_NOTIFICATION rmx_request_notification;
		PFNRMX_INIT_MEM_REGISTRY rmx_init_mem_registry;
		PFNRMX_SET_MEM_REGISTRY_OPTION rmx_set_mem_registry_option;
		PFNRMX_REGISTER_MEMORY rmx_register_memory;
		PFNRMX_DEREGISTER_MEMORY rmx_deregister_memory;
		PFNRMX_INPUT_INIT_STREAM rmx_input_init_stream;
		PFNRMX_INPUT_SET_MEM_CAPACITY_IN_PACKETS rmx_input_set_mem_capacity_in_packets;
		PFNRMX_INPUT_SET_STREAM_NIC_ADDRESS rmx_input_set_stream_nic_address;
		PFNRMX_INPUT_ENABLE_STREAM_OPTION rmx_input_enable_stream_option;
		PFNRMX_INPUT_SET_MEM_SUB_BLOCK_COUNT rmx_input_set_mem_sub_block_count;
		PFNRMX_INPUT_SET_ENTRY_SIZE_RANGE rmx_input_set_entry_size_range;
		PFNRMX_INPUT_DETERMINE_MEM_LAYOUT rmx_input_determine_mem_layout;
		PFNRMX_INPUT_GET_MEM_CAPACITY_IN_PACKETS rmx_input_get_mem_capacity_in_packets;
		PFNRMX_INPUT_GET_MEM_BLOCK_BUFFER rmx_input_get_mem_block_buffer;
		PFNRMX_INPUT_GET_STRIDE_SIZE rmx_input_get_stride_size;
		PFNRMX_INPUT_SET_TIMESTAMP_FORMAT rmx_input_set_timestamp_format;
		PFNRMX_INPUT_CREATE_STREAM rmx_input_create_stream;
		PFNRMX_INPUT_DESTROY_STREAM rmx_input_destroy_stream;
		PFNRMX_INPUT_INIT_FLOW rmx_input_init_flow;
		PFNRMX_INPUT_SET_FLOW_LOCAL_ADDR rmx_input_set_flow_local_addr;
		PFNRMX_INPUT_SET_FLOW_REMOTE_ADDR rmx_input_set_flow_remote_addr;
		PFNRMX_INPUT_SET_FLOW_TAG rmx_input_set_flow_tag;
		PFNRMX_INPUT_ATTACH_FLOW rmx_input_attach_flow;
		PFNRMX_INPUT_DETACH_FLOW rmx_input_detach_flow;
		PFNRMX_INPUT_SET_COMPLETION_MODERATION rmx_input_set_completion_moderation;
		PFNRMX_INPUT_INIT_CHUNK_HANDLE rmx_input_init_chunk_handle;
		PFNRMX_INPUT_GET_NEXT_CHUNK rmx_input_get_next_chunk;
		PFNRMX_INPUT_GET_CHUNK_COMPLETION rmx_input_get_chunk_completion;
		PFNRMX_INPUT_GET_PACKET_INFO rmx_input_get_packet_info;
		PFNRMX_OUTPUT_MEDIA_INIT rmx_output_media_init;
		PFNRMX_OUTPUT_MEDIA_SET_SDP rmx_output_media_set_sdp;
		PFNRMX_OUTPUT_MEDIA_SET_IDX_IN_SDP rmx_output_media_set_idx_in_sdp;
		PFNRMX_OUTPUT_MEDIA_SET_PACKETS_PER_FRAME rmx_output_media_set_packets_per_frame;
		PFNRMX_OUTPUT_MEDIA_SET_SOURCE_PORTS rmx_output_media_set_source_ports;
		PFNRMX_OUTPUT_MEDIA_SET_PCP rmx_output_media_set_pcp;
		PFNRMX_OUTPUT_MEDIA_SET_DSCP rmx_output_media_set_dscp;
		PFNRMX_OUTPUT_MEDIA_SET_ECN rmx_output_media_set_ecn;
		PFNRMX_OUTPUT_MEDIA_INIT_MEM_BLOCKS rmx_output_media_init_mem_blocks;
		PFNRMX_OUTPUT_MEDIA_SET_CHUNK_COUNT rmx_output_media_set_chunk_count;
		PFNRMX_OUTPUT_MEDIA_SET_SUB_BLOCK_COUNT rmx_output_media_set_sub_block_count;
		PFNRMX_OUTPUT_MEDIA_GET_SUB_BLOCK rmx_output_media_get_sub_block;
		PFNRMX_OUTPUT_MEDIA_GET_DUP_SUB_BLOCK rmx_output_media_get_dup_sub_block;
		PFNRMX_OUTPUT_MEDIA_SET_PACKET_LAYOUT rmx_output_media_set_packet_layout;
		PFNRMX_OUTPUT_MEDIA_ASSIGN_MEM_BLOCKS rmx_output_media_assign_mem_blocks;
		PFNRMX_OUTPUT_MEDIA_SET_PACKETS_PER_CHUNK rmx_output_media_set_packets_per_chunk;
		PFNRMX_OUTPUT_MEDIA_SET_STRIDE_SIZE rmx_output_media_set_stride_size;
		PFNRMX_OUTPUT_MEDIA_CREATE_STREAM rmx_output_media_create_stream;
		PFNRMX_OUTPUT_MEDIA_DESTROY_STREAM rmx_output_media_destroy_stream;
		PFNRMX_OUTPUT_MEDIA_INIT_CONTEXT rmx_output_media_init_context;
		PFNRMX_OUTPUT_MEDIA_SET_CONTEXT_BLOCK rmx_output_media_set_context_block;
		PFNRMX_OUTPUT_MEDIA_GET_LOCAL_ADDRESS rmx_output_media_get_local_address;
		PFNRMX_OUTPUT_MEDIA_GET_REMOTE_ADDRESS rmx_output_media_get_remote_address;
		PFNRMX_OUTPUT_MEDIA_INIT_CHUNK_HANDLE rmx_output_media_init_chunk_handle;
		PFNRMX_OUTPUT_MEDIA_SET_CHUNK_PACKET_COUNT rmx_output_media_set_chunk_packet_count;
		PFNRMX_OUTPUT_MEDIA_GET_NEXT_CHUNK rmx_output_media_get_next_chunk;
		PFNRMX_OUTPUT_MEDIA_COMMIT_CHUNK rmx_output_media_commit_chunk;
		PFNRMX_OUTPUT_MEDIA_CANCEL_UNSENT_CHUNKS rmx_output_media_cancel_unsent_chunks;
		PFNRMX_OUTPUT_MEDIA_SKIP_CHUNKS rmx_output_media_skip_chunks;
		PFNRMX_OUTPUT_GEN_INIT_STREAM rmx_output_gen_init_stream;
		PFNRMX_OUTPUT_GEN_SET_PACKETS_PER_CHUNK rmx_output_gen_set_packets_per_chunk;
		PFNRMX_OUTPUT_GEN_SET_LOCAL_ADDR rmx_output_gen_set_local_addr;
		PFNRMX_OUTPUT_GEN_SET_REMOTE_ADDR rmx_output_gen_set_remote_addr;
		PFNRMX_OUTPUT_GEN_SET_MAX_SUB_BLOCKS rmx_output_gen_set_max_sub_blocks;
		PFNRMX_OUTPUT_GEN_SET_PCP rmx_output_gen_set_pcp;
		PFNRMX_OUTPUT_GEN_SET_DSCP rmx_output_gen_set_dscp;
		PFNRMX_OUTPUT_GEN_SET_ECN rmx_output_gen_set_ecn;
		PFNRMX_OUTPUT_GEN_SET_RATE rmx_output_gen_set_rate;
		PFNRMX_OUTPUT_GEN_CREATE_STREAM rmx_output_gen_create_stream;
		PFNRMX_OUTPUT_GEN_DESTROY_STREAM rmx_output_gen_destroy_stream;
		PFNRMX_OUTPUT_GEN_INIT_RATE rmx_output_gen_init_rate;
		PFNRMX_OUTPUT_GEN_SET_RATE_MAX_BURST rmx_output_gen_set_rate_max_burst;
		PFNRMX_OUTPUT_GEN_SET_RATE_TYPICAL_PACKET_SIZE rmx_output_gen_set_rate_typical_packet_size;
		PFNRMX_OUTPUT_GEN_UPDATE_RATE rmx_output_gen_update_rate;
		PFNRMX_OUTPUT_GEN_INIT_CHUNK_HANDLE rmx_output_gen_init_chunk_handle;
		PFNRMX_OUTPUT_GEN_GET_NEXT_CHUNK rmx_output_gen_get_next_chunk;
		PFNRMX_OUTPUT_GEN_SET_CHUNK_REMOTE_ADDR rmx_output_gen_set_chunk_remote_addr;
		PFNRMX_OUTPUT_GEN_APPEND_PACKET_TO_CHUNK rmx_output_gen_append_packet_to_chunk;
		PFNRMX_OUTPUT_GEN_COMMIT_CHUNK rmx_output_gen_commit_chunk;
		PFNRMX_OUTPUT_GET_CHUNK_COUNT rmx_output_get_chunk_count;
		PFNRMX_OUTPUT_UPDATE_DSCP rmx_output_update_dscp;
		PFNRMX_OUTPUT_UPDATE_ECN rmx_output_update_ecn;
		};
bool LoadLibraryFunctions(RIVERMAX_API_FUNCTION_LIST* FunctionList, void* LibraryHandle);
	}