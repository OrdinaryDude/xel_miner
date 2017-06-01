/*
* Copyright 2017 sprocket
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the Free
* Software Foundation; either version 2 of the License, or (at your option)
* any later version.
*/

#define SUPERNODE_VERSION "0.1"

#ifdef WIN32
#define  _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include "miner.h"

#ifndef WIN32
# include <errno.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
#else
#include <ws2tcpip.h>
# include <winsock2.h>
#endif

#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif

#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif

#ifndef in_addr_t
#define in_addr_t uint32_t
#endif

#define MAX_QUEUED_CONNECTIONS 1
#define SOCKET_BUF_SIZE 100000	// Need To Fix This

#ifdef WIN32
	SOCKET s, core;
#else
	long s, core;
#endif


struct req_elasticpl {
	uint32_t req_id;
	char *source;
	bool success;
	char *err_msg;
};

struct req_result {
	uint32_t req_id;
	uint32_t input[12];
	uint32_t state[32];
	bool success;
	char *err_msg;
};



extern void *supernode_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	char ip[] = "127.0.0.1";	// For Now All Connections Are On LocalHost
	unsigned short port = 4016;
	char buf[SOCKET_BUF_SIZE];
	int i, n, len;
	struct sockaddr_in server = { 0 };
	struct sockaddr_in client = { 0 };
	json_t *val;
	json_error_t err;
	uint32_t req_id, req_type;

	applog(LOG_NOTICE, "Starting SuperNode Thread...");

	// Initialize The WebSocket
#ifdef WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		applog(LOG_ERR, "ERROR: Unable to initialize SuperNode socket (%s)", strerror(errno));
		goto out;
	}
#else
	// Need Linux init logic
#endif

	// Create Socket For Core Server To Connect To
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET) {
		applog(LOG_ERR, "ERROR: Unable to create SuperNode socket (%s)", strerror(errno));
		goto out;
	}

	// Prepare Socket Address For SuperNode
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	server.sin_addr.s_addr = inet_addr(ip);
	server.sin_port = htons(port);
	if (server.sin_addr.s_addr == (in_addr_t)INADDR_NONE) {
		applog(LOG_ERR, "ERROR: Unable to configure SuperNode IP address (%s)", strerror(errno));
		goto out;
	}

	// Bind Socket To SuperNode Port (6 Tries Max)
	applog(LOG_DEBUG, "DEBUG: Binding Socket to port %d", port);
	for (i = 0; i < 10; i++) {
		n = bind(s, (struct sockaddr *)(&server), sizeof(server));
		if (n >= 0)
			break;
		sleep(10);
	}

	if (n < 0) {
		applog(LOG_ERR, "ERROR: Unable to bind Socket to port $d (%s)", port, strerror(errno));
		goto out;
	}

	// Listen For Incoming Connections
	if (listen(s, MAX_QUEUED_CONNECTIONS) < 0) {
		applog(LOG_ERR, "ERROR: Unable to set Queue for SuperNode socket (%s)", strerror(errno));
		goto out;
	}


	// Main Loop To Reconnect If Connection Is Lost
	while (1) {

		// Accept Connection From Core Server
		len = sizeof(struct sockaddr_in);
		core = accept(s, (struct sockaddr *)&client, &len);
		if (core < 0) {
			applog(LOG_ERR, "ERROR: Unable to accept connection to SuperNode (%s)", strerror(errno));
			goto out;
		}

		if (client.sin_addr.S_un.S_addr == server.sin_addr.S_un.S_addr)
			applog(LOG_NOTICE, "SuperNode connected to Elastic Core Server");
		else {
			applog(LOG_ERR, "ERROR: SuperNode blocked connection from IP: %s", inet_ntoa(client.sin_addr));
			continue;
		}

		// Secondary Loop To Read Requests From Core Server
		while (1) {

			// Read Request Into Buffer
			n = recv(core, &buf[0], SOCKET_BUF_SIZE, 0);

			// Reset If There Is A Connection Error
			if (n <= 0)
				break;

			// Make Sure Request Is Zero Terminated
			buf[n] = '\0';

			// Get Request ID / Type From JSON
			val = JSON_LOADS(buf, &err);
			if (!val) {
				applog(LOG_ERR, "ERROR: JSON decode failed (code=%d): %s", err.line, err.text);

				// Send Acknowledgment To Core Server (Error)
				n = send(core, "ERROR", 6, 0);
			}
			else {
				// Send Acknowledgment To Core Server (OK)
				n = send(core, "OK", 3, 0);

//				if (opt_protocol) {
					char *str = json_dumps(val, JSON_INDENT(3));
					applog(LOG_DEBUG, "DEBUG: JSON SuperNode Request -\n%s", str);
					free(str);
//				}

				req_id = (uint32_t)json_integer_value(json_object_get(val, "req_id"));
				req_type = (uint32_t)json_integer_value(json_object_get(val, "req_type"));

				applog(LOG_DEBUG, "DEBUG: Req_Id: %d, Req_Type: %d Received", req_id, req_type);
			}

			if (req_type == 2) { // Validate ElasticPL Syntax

				struct req_elasticpl req;
				req.req_id = req_id;
				req.source = strdup((char *)json_string_value(json_object_get(val, "source")));

				// Push Request To Validate ElasticPL Queue
				tq_push(thr_info[1].q, &req);
			}

			json_decref(val);

// TODO Create Queues For Each Req Type

// TODO Add Mutex Around Send

		}

#ifdef WIN32
		if (core != INVALID_SOCKET)
			closesocket(core);
#else
		if (core)
			close(core);
#endif

	}

out:

#ifdef WIN32
	if (s != INVALID_SOCKET)
		closesocket(s);
#else
	if (s)
		close(s);
#endif

	tq_freeze(mythr->q);
	return NULL;
}

extern void *sn_validate_elasticpl_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	struct req_elasticpl *req;
	char msg[512];
	char *elastic_src = NULL;
	int n, rc;

	elastic_src = malloc(MAX_SOURCE_SIZE);
	if (!elastic_src) {
		applog(LOG_ERR, "ERROR: Unable to allocate memory for ElasticPL Source");
		return NULL;
	}

	while (1) {

		// Check For New Requests On Queue
		req = (struct req_elasticpl *) tq_pop(mythr->q, NULL);

		applog(LOG_DEBUG, "Validating ElasticPL (req_id: %d)", req->req_id);

		rc = ascii85dec(elastic_src, MAX_SOURCE_SIZE, req->source);
		if (!rc) {

			// TODO - Get Error Message & Send
		}

		// Validate ElasticPL For Syntax Errors
		if (!create_epl_vm(elastic_src, NULL)) {
			applog(LOG_DEBUG, "DEBUG: ...Req_id: %d)", req->req_id);

			// TODO - Get Error Message & Send
		}

		// Calculate WCET
		if (!calc_wcet()) {
			applog(LOG_DEBUG, "DEBUG: ....Req_id: %d)", req->req_id);

			// TODO - Get Error Message & Send
		}

		// TODO - Add Mutex Around Send
		sprintf(msg, "{\"req_id\": %lu,\"req_type\": %lu,\"sucess\": %lu,\"error\": \"%s\"}", req->req_id, 2, 1, "");
		n = send(core, msg, strlen(msg), 0);

		// TODO - Add Error Handling

	}

	tq_freeze(mythr->q);
	return NULL;
}

extern void *sn_validate_result_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;

	while (1) {
		;
	}

	tq_freeze(mythr->q);
	return NULL;
}