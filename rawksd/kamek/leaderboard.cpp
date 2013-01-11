#include "rb2.h"
#include <ogc/machine/processor.h>
#include <ogc/es.h>
#include <network.h>

#define LEADERBOARD_ENABLED() \
	(*(u32*)0x80002FFC == 0x1337BAAD)

static const char asciitohex[16] = "0123456789ABCDEF";

static char* ToHttpString(const char* str, int length = 0)
{
	if (!length)
		length = strlen(str);
	char *dest, *d;
	dest = d = (char*)malloc(length * 3 + 1);

	for (int i = 0; i < length; i++) {
		char chr = str[i];
		if ((chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || (chr >= '0' && chr <= '9')) {
			*d++ = chr;
		} else {
			*d++ = '%';
			*d++ = asciitohex[(chr>>4)&0xF];
			*d++ = asciitohex[chr&0xF];
		}
	}

	*d = '\0';
	return dest;
}

static bool SubmitLeaderboardRawkSD(Symbol* symbol, int instrument, int difficulty, int score)
{
	if (!LEADERBOARD_ENABLED())
		return false;
	if (!ThePlatformMgr.IsConnected)
		return false;

	const char* hostname = "rvlution.net";
	char* username;
	char* songname;
	char* fullsongname;
	char* artistname;
	char* message = NULL;
	u32 console = 0;
	int socket = 0;
	int ret = 0;
	struct sockaddr_in address;
	hostent* host;
	int size = 0;
	u32 checksum = 0;
	const char* song = symbol->name;
	const char* originalsongname = gSongMgrWii.SongName(symbol);
	bool hasname = originalsongname && strlen(originalsongname);

	memset(&address, 0, sizeof(address));
	address.sin_family = PF_INET;
	address.sin_len = 8;

	// FIXME: should use the friend code instead of console ID to handle wii->wiiu migration
	const char* fullusername = ThePlatformMgr.GetUsernameFull(); // Full username is in the format "RB2OnlineName;consolefriendcode"
	char* colon = strrchr(fullusername, ';');

	if (ES_GetDeviceID(&console) < 0) {
		OSReport("RawkSD: Error getting ID\n");
		goto onerror;
	}

	host = net_gethostbyname(hostname);
	if (!host || host->h_length != sizeof(address.sin_addr) || host->h_addrtype != PF_INET || host->h_addr_list == NULL || host->h_addr_list[0] == NULL) {
		OSReport("RawkSD: Host lookup failed.\n");
		goto onerror;
	}
	memcpy(&address.sin_addr, host->h_addr_list[0], host->h_length);
	address.sin_port = htons(80);

	socket = net_socket(PF_INET, SOCK_STREAM, 0);
	if (socket < 0) {
		OSReport("RawkSD: Socket creation failed. %d\n", socket);
		goto onerror;
	}
	ret = net_connect(socket, (struct sockaddr*)&address, sizeof(address));
	if (ret < 0) {
		OSReport("RawkSD: Connection failed. %d\n", ret);
		net_close(socket);
		goto onerror;
	}

	username = ToHttpString(fullusername, colon ? (colon - fullusername) : 0);
	songname = ToHttpString(song);
	fullsongname = ToHttpString(originalsongname);
	artistname = ToHttpString(gSongMgrWii.SongArtist(symbol));

	checksum = instrument << 3;
	checksum |= difficulty;
	checksum ^= console >> 4;
	checksum ^= ~score << 6;
	for (u32 i = 0; i < strlen(song); i++)
		checksum ^= (u32)song[i] << 24;

	message = (char*)malloc(0x100 + strlen(username) + strlen(songname) + strlen(fullsongname) + strlen(artistname) + strlen(hostname));
	sprintf(message, "GET /rb2/post?inst=%d&diff=%d&score=%d&console=%d&nickname=%s&songid=%s&name=%s&artist=%s&valid=%d HTTP/1.1\r\nHost: %s\r\n\r\n\r\n", instrument, difficulty, score, console, username, songname, fullsongname, artistname, checksum, hostname);
	free(username);
	free(songname);
	free(fullsongname);
	free(artistname);
	size = strlen(message);
	ret = net_send(socket, message, size, 0);
	net_close(socket);
	if (ret != size) {
		OSReport("RawkSD: Send failed. %d\n", ret);
		goto onerror;
	}

	OSReport("RawkSD: Leaderboard submission for \"%s\" succeeded.\n", song);
	if (!message)
		message = (char*)malloc(0x100);
	sprintf(message, "Your new high score%s%s been sent to the RawkSD Leaderboards!", hasname ? " for " : "", hasname ? originalsongname : "");
	GetPMPanel()->QueueMessage(message);
	free(message);
	return true;

onerror:
	OSReport("RawkSD: Leaderboard submission for \"%s\" failed! :(\n", song);
	if (!message)
		message = (char*)malloc(0x100);
		sprintf(message, "An error occurred uploading your high score%s%s to RawkSD.", hasname ? " for " : "", hasname ? originalsongname : "");
	GetPMPanel()->QueueMessage(message);
	free(message);
	return false;
}

extern "C" void SoloLeaderboardHack(RockCentralGateway* gateway, Symbol* symbol, int instrument, int difficulty, int score1, int score2, int player, Hmx::Object* object)
{
	gateway->SubmitPlayerScore(symbol, instrument, difficulty, score1, score2, player, object);
	SubmitLeaderboardRawkSD(symbol, instrument, difficulty, score1);
}

extern "C" void BandLeaderboardHack(RockCentralGateway* gateway, Symbol* symbol, int score1, int score2, int fans, HxGuid* guid, Hmx::Object* object)
{
	gateway->SubmitBandScore(symbol, score1, score2, fans, guid, object);
	SubmitLeaderboardRawkSD(symbol, 4, 0, score1);
}

