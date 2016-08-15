/*
 * howsit.c
 *
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <ncurses.h>
#include <assert.h>
#include <getopt.h>

#define BUF_SIZE 64*1024
#define MAX_SLABS 100
#define MAX_SLABS_PER_PAGE 20
#define PAGE_SIZE 1024*1024
#define WARN_THRESH 1000
#define REFRESH_SECONDS 5

#define COL1 0
#define COL2 10
#define COL3 18
#define COL4 30
#define COL5 40
#define COL6 54
#define COL7 66
#define COL8 78
#define COL9 92

void print_usage() 
{
	fprintf(stderr, "Usage:\n"
		"-h, --help        show help\n"
	    "-v, --version     show version info\n"
	    "-r, --refresh=N   refresh every N seconds, default is 5\n"
	    "-s, --server=S    memcached host, default is 'localhost'\n"
	    "-p, --port=N      memcached port, default is 11211\n"
	    "-m, --max_slabs=N maximum number of slabs to show at once, default is 20\n");
}

static struct option long_options[] = {
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'v'},
	{ "refresh", required_argument, NULL, 'r'},
	{ "server", required_argument, NULL, 's'},
	{ "port", required_argument, NULL, 'p'},
	{ "max_slabs", required_argument, NULL, 'm'}
};

static char short_options[] = "hvr:s:p:m:";

typedef struct {
	long long value;
	long long value_prev;
	long double rate;
} rate;

typedef struct stats_slabs {
	int slab;
	long long chunk_size;
	long long total_pages;
	long long used_chunks;
	long long free_chunks;
	long long mem_requested;
	rate cmd_set;
	rate get_hits;
} SS;

static const SS empty_stats_slabs;

typedef struct stats_items {
	int slab;
	long long number;
	rate evicted;
	long long evicted_time;
} SI;

static const SI empty_stats_items;

typedef struct stats {
	char* server;
	char* port;
	long long time;
	long long time_prev;
	long long uptime;
	char version[10];
	long long limit_maxbytes;
	rate cmd_get;
	rate cmd_set;
	rate evictions;
	long long total_items;
	long long get_hits;
	SI si[MAX_SLABS];
	SS ss[MAX_SLABS];
	bool show_rates;
	int start_slab;
	bool more_slabs;
	int last_slab_shown;
	int refresh_seconds;
	int max_slabs_per_page;
} STATS;

static const STATS empty_stats;

char* get_from_file(char* buf, char* name)
{
	FILE* f = fopen (name, "r");
	size_t read = fread(buf, 1, BUF_SIZE, f);
	buf[read]='\0';
	fclose(f);
	return buf;
}

char* format_bytes(char* buf, long long bytes)
{
	if (bytes < 1024)
		sprintf(buf, "%lld", bytes);
	else if (bytes < 1024*1024)
		sprintf(buf, "%.1LfK", (long double)bytes/1024.0);
	else if (bytes < 1024*1024*1024)
		sprintf(buf, "%.1LfM", (long double)bytes/1024.0/1024.0);
	else
		sprintf(buf, "%.1LfG", (long double)bytes/1024.0/1024.0/1024.0);
	return buf;
}

/*
 *	Returns current time in milliseconds since the epoch.
 *	todo: replace with monotonic clock?
 */
long long current_timestamp() 
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec*1000 + tv.tv_usec/1000;
}

void fill_stats_vals(const char *name, const char *value, STATS* stats) 
{
	if (strcmp("uptime", name) == 0) {
		stats->uptime = atoll(value);
	} else if (strcmp("version", name) == 0) {
		strcpy(stats->version, value);
	} else if (strcmp("limit_maxbytes", name) == 0) {
		stats->limit_maxbytes=atoll(value);
	} else if (strcmp("total_items", name) == 0) {
		stats->total_items=atoll(value);
	} else if (strcmp("evictions", name) == 0) {
		stats->evictions.value=atoll(value);
		stats->evictions.rate = 
			(long double)(stats->evictions.value-stats->evictions.value_prev)/
			((long double)(stats->time-stats->time_prev)/1000.0L);
		stats->evictions.value_prev = stats->evictions.value;
	} else if (strcmp("cmd_get", name) == 0) {
		stats->cmd_get.value=atoll(value);
		stats->cmd_get.rate = 
			(long double)(stats->cmd_get.value-stats->cmd_get.value_prev)/
			((long double)(stats->time-stats->time_prev)/1000.0L);
		stats->cmd_get.value_prev = stats->cmd_get.value;
	} else if (strcmp("cmd_set", name) == 0) {
		stats->cmd_set.value=atoll(value);
		stats->cmd_set.rate = 
			(long double)(stats->cmd_set.value-stats->cmd_set.value_prev)/
			((long double)(stats->time-stats->time_prev)/1000.0L);
		stats->cmd_set.value_prev = stats->cmd_set.value;
	} else if (strcmp("get_hits", name) == 0) {
		stats->get_hits=atoll(value);
	}
}

void fill_stats_element(char* line, STATS* stats)
{
	char name[64], value[64];
	char *token, *split = " ", *save;

	token = strtok_r(line, split, &save);
	if (strncmp(token, "STAT", 3) != 0) {
		return;
	}	

	token = strtok_r(NULL, split, &save);
	if (token == NULL) return;
	strcpy(name, token);

	token = strtok_r(NULL, split, &save);
	if (token == NULL) return;
	strcpy(value, token);

	fill_stats_vals(name, value, stats);	

}

void fill_stats_items_vals(const char *name, const char *value, int slab, STATS* s)
{
	SI* si = &s->si[slab];
	si->slab = slab+1;
	if (strcmp("evicted_time", name) == 0) {
		si->evicted_time = atoll(value);
	} else if (strcmp("evicted", name) == 0) {
		si->evicted.value=atoll(value);
		si->evicted.rate = 
			(long double)(si->evicted.value-si->evicted.value_prev)/
			((long double)(s->time-s->time_prev)/1000.0L);
		si->evicted.value_prev=si->evicted.value;
	}
}

void fill_stats(char* input, STATS* stats)
{
	char *token, *split="\n\r";
	token = strtok(input, split);
	while (token != NULL) {
		fill_stats_element(token, stats);
		token = strtok(NULL, split);
	}	
}

void fill_stats_items_element(char* line, STATS* stats)
{
	char name[64], value[64];
	char *token, *split = " ", *save, *save2;
	int slab=-1;

	token = strtok_r(line, split, &save);
	if (strncmp(token, "STAT", 3) != 0) {
		return;
	}

	token = strtok_r(NULL, split, &save);
	if (token == NULL) return;
	token = strtok_r(token,":", &save2);
	token = strtok_r(NULL, ":", &save2);
	slab = atoi(token);
	token = strtok_r(NULL, ":", &save2);
	strcpy(name, token);

	token = strtok_r(NULL, split, &save);
	if (token == NULL) return;
	strcpy(value, token);
	assert(slab>0);
	fill_stats_items_vals(name, value, slab-1, stats);	
}

void fill_stats_items(char* input, STATS* s)
{
	char *token, *split="\n\r";
	int i;

	for (i=0; i<MAX_SLABS; i++) {
		// set all slabs to deactivated as default
		s->si[i].slab=-1;
	}

	token = strtok(input, split);
	while (token != NULL) {
		fill_stats_items_element(token, s);
		token = strtok(NULL, split);
	}	
}

void fill_stats_slabs_vals(const char *name, const char *value, int slab, STATS* s)
{
	SS* ss = &s->ss[slab];	
	ss->slab = slab+1;

	if (strcmp("chunk_size", name) == 0) {
		ss->chunk_size = atoll(value);
	} else if (strcmp("total_pages", name) == 0) {
		ss->total_pages=atoll(value);
	} else if (strcmp("mem_requested", name) == 0) {
		ss->mem_requested=atoll(value);
	} else if (strcmp("used_chunks", name) == 0) {
		ss->used_chunks=atoll(value);
	} else if (strcmp("cmd_set", name) == 0) {
		ss->cmd_set.value=atoll(value);
		ss->cmd_set.rate = 
			(long double)(ss->cmd_set.value-ss->cmd_set.value_prev)/
			((long double)(s->time-s->time_prev)/1000.0L);
		ss->cmd_set.value_prev=ss->cmd_set.value;		
	} else if (strcmp("get_hits", name) == 0) {
		ss->get_hits.value = atoll(value);
		ss->get_hits.rate= 
			(long double)(ss->get_hits.value-ss->get_hits.value_prev)/
			((long double)(s->time-s->time_prev)/1000.0L);
		ss->get_hits.value_prev=ss->get_hits.value;
	}
}

void fill_stats_slabs_element(char* line, STATS* s)
{
	char name[64], value[64];
	char *token, *split = " ", *save, *save2;
	int slab=-1;

	token = strtok_r(line, split, &save);
	if (strncmp(token, "STAT", 3) != 0) {
		return;
	}

	token = strtok_r(NULL, split, &save);
	if (token == NULL) return;
	// this piece looks like -> '1:chunk_size' or 'active_slabs'
	if (strstr(token, ":") == NULL) {
		return;
	}

	token = strtok_r(token,":", &save2);
	slab = atoi(token);
	token = strtok_r(NULL, ":", &save2);
	strcpy(name, token);

	token = strtok_r(NULL, split, &save);
	if (token == NULL) return;
	strcpy(value, token);
	assert(slab>0);

	fill_stats_slabs_vals(name, value, slab-1, s);
}

void fill_stats_slabs(char* input, STATS* s)
{
	char *token, *split="\n\r";
	int i;
	for (i=0; i<MAX_SLABS; i++) {
		// set all slabs to deactivated as default
		s->ss[i].slab=-1;
	}

	token = strtok(input, split);
	while (token != NULL) {
		fill_stats_slabs_element(token, s);
		token = strtok(NULL, split);
	} 
}

void make_call(char* response, const char* command, const char* server, const char* port)
{
	int sockfd, result;
	struct addrinfo hints, *res;
	char sendline[512];
	int RECV_BUF_SIZE = 1024;
	char recvbuffer[RECV_BUF_SIZE];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	result = getaddrinfo(server, port, &hints, &res);
	if (result != 0) {
		printf("getaddrinfo failed\n");
		endwin();
		exit(1);
	}

	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd == -1) {
		printf("couldn't create socket\n");
		endwin();
		exit(1);
	}

	result = connect(sockfd, res->ai_addr, res->ai_addrlen);
	if (result != 0) {
		printf("%d: could not connection properly\n", result);
		endwin();
		exit(1);
	}

	sprintf(sendline, "%s\r\n", command);

	result = send(sockfd, sendline, strlen(sendline), 0);
	if (result != strlen(sendline)) {
		printf("sendto failed\n");
		endwin();
		exit(1);
	}

	response[0]='\0';

	while((result = recv(sockfd, recvbuffer, RECV_BUF_SIZE-1, 0)) > 0) {
		if (result < 0) {
			int errsv = errno;
			printf("%d: recv failed\n", errsv);
			exit(1);
		}
		recvbuffer[result]='\0';
		strcat(response, recvbuffer);
		// todo: fix sloppy
		if (strstr(recvbuffer,"END")!= NULL)
			break;
	}

	freeaddrinfo(res);
	close(sockfd);

	return;
}

void draw_screen(STATS* stats)
{
	int row=0, i=0;
	char buf[64];
	
	mvprintw(row, COL1, "MC SERVER:%s PORT:%s VERSION:(%s) MEMORY:%s UPTIME:%lld REFRESH RATE:%ds",
	         stats->server, stats->port, stats->version, 
	         format_bytes(buf, stats->limit_maxbytes),
	         stats->uptime,
	         stats->refresh_seconds);
	row++;
	mvprintw(row, COL1, "SLAB");
	mvprintw(row, COL2, "SIZE");
	mvprintw(row, COL3, "USED");
	mvprintw(row, COL4, "PAGES");
	mvprintw(row, COL5, "WASTED");
	mvprintw(row, COL6, "EVICT_AGE");
	if (stats->show_rates) {
		mvprintw(row, COL7, "EVICTED/s");
		mvprintw(row, COL8, "SET/s");
		mvprintw(row, COL9, "HIT/s");
	} else {
		mvprintw(row, COL7, "EVICTED");
		mvprintw(row, COL8, "SET");
		mvprintw(row, COL9, "HIT");
	}
	
	for (i=stats->start_slab; i<MAX_SLABS; i++) {
		if (stats->ss[i].slab == -1) {
			continue;
		}
		if (row>stats->max_slabs_per_page) {
			stats->more_slabs=TRUE;
			stats->last_slab_shown=i-1;
			break;
		}
		row++;
		mvprintw(row, COL1, "%d", stats->ss[i].slab);
		mvprintw(row, COL2, "%lld", stats->ss[i].chunk_size);
		mvprintw(row, COL3, "%lld", stats->ss[i].used_chunks);
		mvprintw(row, COL4, "%lld", stats->ss[i].total_pages);
		long long wasted = (stats->ss[i].total_pages*PAGE_SIZE)-stats->ss[i].mem_requested;
		mvprintw(row, COL5, "%s", format_bytes(buf, wasted));
		if (stats->si[i].evicted_time > 0 && stats->si[i].evicted_time < WARN_THRESH) {
			attron(A_BOLD);
			attron(COLOR_PAIR(1));
		}
		mvprintw(row, COL6, "%lld", stats->si[i].evicted_time);
		attroff(A_BOLD);
		attroff(COLOR_PAIR(1));
		if (stats->show_rates) {
			mvprintw(row, COL7, "%.1llf", stats->si[i].evicted.rate);
			mvprintw(row, COL8, "%.1llf", stats->ss[i].cmd_set.rate);
			mvprintw(row, COL9, "%.1llf", stats->ss[i].get_hits.rate);
		} else {
			mvprintw(row, COL7, "%lld", stats->si[i].evicted.value);
			mvprintw(row, COL8, "%lld", stats->ss[i].cmd_set.value);
			mvprintw(row, COL9, "%lld", stats->ss[i].get_hits.value);
		}
	}
	// print general stats info for the server
	if (stats->show_rates) {
		mvprintw(++row, COL1, "EVICTIONS/s: %.1llf", stats->evictions.rate);
		mvprintw(++row, COL1, "SETS/s: %.1llf", stats->cmd_set.rate);
		mvprintw(++row, COL1, "GETS/s: %.1llf", stats->cmd_get.rate);
	} else {
		mvprintw(++row, COL1, "EVICTIONS: %lld", stats->evictions);
		mvprintw(++row, COL1, "SETS: %lld", stats->cmd_set);
		mvprintw(++row, COL1, "GETS: %lld", stats->cmd_get);
	}
	mvprintw(++row, COL1, "HIT RATIO: %.2llf", 
	         (long double)stats->get_hits/(long double)stats->cmd_get.value);

}

void check_options(int argc, char** argv, STATS* stats)
{
	int c;

	stats->server="localhost";
	stats->port="11211";

	strcpy(stats->version, "N/A");
	stats->refresh_seconds=REFRESH_SECONDS;
	stats->max_slabs_per_page=MAX_SLABS_PER_PAGE;
	
	while(1) {
		c = getopt_long(argc, argv, short_options, long_options, NULL);
		if (c == -1) {
			break;
		}
		
		switch (c) {
			case 'h':
				print_usage();
				exit(0);
			case 'v':
				printf("version .1\n");
				exit(0);
			case 'r':
				stats->refresh_seconds=atoi(optarg);
				break;
		 	case 's':
				stats->server=optarg;
				break;
			case 'p':
				stats->port=optarg;
				break;
			case 'm':
				stats->max_slabs_per_page=atoi(optarg);
				break;
			default:
            	fprintf(stderr, "Usage: %s [-t nsecs] [-n] name\n",
                    argv[0]);
				exit(1);
		}
	}
}

void load_stats(STATS* stats, bool load_from_file) 
{
	char recvline[BUF_SIZE];

	assert(stats != NULL);

	stats->time=current_timestamp();
	if (load_from_file) {
		fill_stats_slabs(get_from_file(recvline,"test_slabs_data.txt"), stats);
		fill_stats_items(get_from_file(recvline,"test_items_data.txt"), stats);
		fill_stats(get_from_file(recvline,"test_stats.txt"), stats);
	} else {
		make_call(recvline, "stats slabs", stats->server, stats->port);
		fill_stats_slabs(recvline, stats);
		make_call(recvline, "stats items", stats->server, stats->port);
		fill_stats_items(recvline, stats);
		make_call(recvline, "stats", stats->server, stats->port);
		fill_stats(recvline, stats);		
	}
	stats->time_prev = stats->time;
}


int main(int argc, char**argv)
{
	bool load_from_file=false;
	struct timespec tm;
	long long last_loaded_millis;
	
	tm.tv_sec=0;
	tm.tv_nsec=150000000; // .150 second

	STATS stats=empty_stats;
	check_options(argc, argv, &stats);

	// ncursors init	
	initscr();
	halfdelay(1);
	noecho();
	start_color();
	use_default_colors();
	init_pair(1, COLOR_YELLOW, -1);

	// first call to load stats
	load_stats(&stats, load_from_file);
	last_loaded_millis = current_timestamp();

	// main loop
	while (1) {
		// update stats by calling server periodically
		if (current_timestamp() > (last_loaded_millis+(stats.refresh_seconds*1000L))) {
			load_stats(&stats, load_from_file);
			last_loaded_millis=current_timestamp();
			if (stats.more_slabs) {
				stats.start_slab=stats.last_slab_shown+1;
				stats.more_slabs=FALSE;
				if (stats.start_slab>=MAX_SLABS) {
					stats.start_slab=0;
				}
			} else {
				stats.start_slab=0;
			}
		}
		clear();
		draw_screen(&stats);
		refresh();
		int val = getch();
		if (val == 'r') {
			stats.show_rates = !stats.show_rates;
		} else if (val == 'q') {
			endwin();
			exit(0);
		}
		nanosleep(&tm, NULL);
	}

	endwin();
	exit(0);
}
