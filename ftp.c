#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <git2/errors.h>
#include <git2/global.h>
#include <git2/oid.h>
#include <git2/repository.h>
#include <git2/revparse.h>
#include <git2/tree.h>

#include <sys/socket.h>

#include "socket.h"

#define CLIENT_BUFSZ (10+PATH_MAX)

void git_or_die(FILE *conn, int code)
{
	if (code < 0)
	{
		fprintf(conn, "451 libgit2 error: %s\n", giterr_last()->message);
		exit(EXIT_FAILURE);
	}
}

/* wrapper to match expected atexit type */
void cleanup_git(void)
{
	git_libgit2_shutdown();
}

void ftp_ls(FILE *conn, git_tree *tr)
{
	size_t i, n = git_tree_entrycount(tr);
	const char *name;
	git_tree_entry *entry;

	for (i = 0; i < n; ++i)
	{
		 entry = (git_tree_entry *)git_tree_entry_byindex(tr, i);
		 name = git_tree_entry_name(entry);
		 fprintf(conn, "%s\n", name);
	}
}

void pasv_format(const int *ip, int port, char *out)
{
	div_t p = div(port, 256);

	sprintf(out, "(%d,%d,%d,%d,%d,%d)",
			ip[0], ip[1], ip[2], ip[3],
			p.rem, p.quot);
}

void ftp_session(int sock, int *server_ip, const char *gitpath)
{
	char sha[8];
	char cmd[CLIENT_BUFSZ];

	int pasvfd = -1, pasvport;
	FILE *conn, *pasv_conn = NULL;
	char pasv_desc[26]; /* format (%d,%d,%d,%d,%d,%d) */

	git_repository *repo;
	git_object *obj;
	git_tree *tr;

	if ((conn = sock_stream(sock, "a+")) == NULL)
		exit(EXIT_FAILURE);

	git_or_die(conn, git_libgit2_init());
	atexit(cleanup_git);

	git_or_die(conn, git_repository_open(&repo, gitpath) );
	git_or_die(conn, git_revparse_single((git_object **)&obj, repo, "HEAD^{tree}") );
	tr = (git_tree *)obj;

	fprintf(conn, "220 Browsing at SHA (%s)\n",
	        git_oid_tostr(sha, sizeof sha, git_object_id((git_object*)tr)));
	while (fgets(cmd, CLIENT_BUFSZ, conn) != NULL)
	{
		printf("<< %s", cmd);
		if (strncmp(cmd, "USER", 4) == 0)
			fprintf(conn, "331 Username OK, supply any pass\n");
		else if (strncmp(cmd, "PASS", 4) == 0)
			fprintf(conn, "230 Logged in\n");
		else if (strncmp(cmd, "PWD", 3) == 0)
			fprintf(conn, "257 \"/\"\n");
		else if (strncmp(cmd, "CWD", 3) == 0)
			fprintf(conn, "250 Smile and nod\n");
		else if (strncmp(cmd, "LIST", 4) == 0)
		{
			if (pasvfd < 0)
			{
				fprintf(conn, "425 Use PASV first\n");
				continue;
			}

			puts("Listing requested, accepting");
			if ((pasv_conn = sock_stream(accept(pasvfd, NULL, NULL), "w")) == NULL)
			{
				fprintf(conn, "452 Failed to accept() pasv sock\n");
				continue;
			}
			fprintf(conn, "150 Opening ASCII mode data connection for file list\n");
			ftp_ls(pasv_conn, tr);
			fclose(pasv_conn);
			pasvfd = -1;
			fprintf(conn, "226 Directory finished\n");
		}
		else if (strncmp(cmd, "SYST", 4) == 0)
			fprintf(conn, "215 git\n");
		else if (strncmp(cmd, "TYPE", 4) == 0)
			fprintf(conn, "200 Sure whatever\n");
		else if (strncmp(cmd, "QUIT", 4) == 0)
		{
			fprintf(conn, "250 Bye\n");
			break;
		}
		else if (strncmp(cmd, "PASV", 4) == 0)
		{
			/* ask system for random port */
			pasvfd = negotiate_listen("0");
			if (pasvfd < 0)
			{
				fprintf(conn, "452 Passive mode port unavailable\n");
				continue;
			}
			if (get_ip_port(pasvfd, NULL, &pasvport) < 0)
			{
				close(pasvfd);
				pasvfd = -1;
				fprintf(conn, "452 Passive socket incorrect\n");
				continue;
			}
			pasv_format(server_ip, pasvport, pasv_desc);
			printf("Opening passive socket on %s\n", pasv_desc);

			fprintf(conn, "227 Entering Passive Mode %s\n", pasv_desc);
		}
		else
			fprintf(conn, "502 Unimplemented\n");
	}
	fputs("Client disconnected\n", stderr);
	if (pasv_conn != NULL)
		fclose(pasv_conn);
	git_tree_free(tr);
}
