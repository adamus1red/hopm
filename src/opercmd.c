/*
 *  Copyright (c) 2002 Erik Fears
 *  Copyright (c) 2014-2015 ircd-hybrid development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 *  USA
 */

#include "setup.h"

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

#include "options.h"
#include "irc.h"
#include "log.h"
#include "main.h"
#include "config.h"
#include "opercmd.h"
#include "scan.h"
#include "memory.h"
#include "list.h"
#include "stats.h"


static list_t *COMMANDS = NULL;  /* List of active commands */


static struct Command *command_create(const struct OperCommandHash *, char *param, char *irc_nick, const struct ChannelConf *target);
static void command_free(struct Command *);

static void cmd_check(char *, const struct ChannelConf *);
static void cmd_stat(char *, const struct ChannelConf *);
static void cmd_fdstat(char *, const struct ChannelConf *);

static const struct OperCommandHash COMMAND_TABLE[] =
{
  { "CHECK",  cmd_check  },
  { "SCAN",   cmd_check  },
  { "STAT",   cmd_stat   },
  { "STATS",  cmd_stat   },
  { "STATUS", cmd_stat   },
  { "FDSTAT", cmd_fdstat },
  { NULL,     NULL       }
};


/* command_init
 * 
 *    Do command initialization
 *
 * Parameters: NONE
 * Return: NONE
 *
 */
void
command_init(void)
{
  if (COMMANDS == NULL)
    COMMANDS = list_create();
}

/* command_timer
 *
 *    Perform ~1 second actions.
 *
 * Parameters: NONE
 * 
 * Return: NONE
 *
 */
void
command_timer(void)
{
  static unsigned int interval;
  node_t *node, *node_next;
  time_t present;

  /* Only perform command removal every COMMANDINTERVAL seconds */
  if (interval++ < COMMANDINTERVAL)
    return;
  else
    interval = 0;

  time(&present);

  LIST_FOREACH_SAFE(node, node_next, COMMANDS->head)
  {
    struct Command *cs = node->data;

    if ((present - cs->added) > COMMANDTIMEOUT)
    {
      command_free(cs);
      list_remove(COMMANDS, node);
      node_free(node);
    }
    else  /* Since the queue is in order, it's also ordered by time, no nodes after this will be timed out */
      return;
  }
}

/* command_parse
 *
 *    Parse a command to hopm (sent to a channel hopm is on). The command is parsed
 *    from the parameters, and if it is a known command it is stored in a queue. A
 *    userhost is performed on the user to check if they are an IRC operator. When
 *    a reply is returned (command_userhost), the command will be executed.
 *
 * Parameters:
 *    command: Command sent (including parameters)
 *    target: Channel command was sent to (we only got this far if there was only one recipient)
 *    source_p: Operator (hopefully) that sent the command.
 *
 */
void
command_parse(char *command, const struct ChannelConf *target,
              const struct UserInfo *source_p)
{
  char *param;  /* Parsed parameters */

  if (OPT_DEBUG)
    log_printf("COMMAND -> Parsing command (%s) from %s [%s]", command,
               source_p->irc_nick, target->name);

  /* Only allow COMMANDMAX commands in the queue */
  if (LIST_SIZE(COMMANDS) >= COMMANDMAX)
    return;

  /*
   * Parameter is the first character in command after the first space.
   * param will be NULL if:
   * 1. There was no space
   * 2. There was a space but it was the last character in command, in which case
   *    param = '\0'
   */

  /* Skip past the botname/!all */
  command = strchr(command, ' ');

  /* TBD: skip leading spaces if there's more than one */
  /*
   * There is no command OR there is at least nothing
   * past that first space.
   */
  if (command == NULL || *++command == '\0')
    return;

  /* Find the parameters */
  param = strchr(command, ' ');

  if (param)
  {
    *param = '\0';
    param++;
  }

  log_printf("COMMAND -> parsed [%s] [%s]", command, param ? param : "");

  /* Lookup the command in the table */
  for (const struct OperCommandHash *tab = COMMAND_TABLE; tab->command; ++tab)
  {
    if (strcasecmp(command, tab->command) == 0)
    {
      /* Queue this command */
      struct Command *cs = command_create(tab, param, source_p->irc_nick, target);

      list_add(COMMANDS, node_create(cs));
      break;
    }
  }

  irc_send("USERHOST %s", source_p->irc_nick);
}

/* command_create
 * 
 *    Create a Command struct.
 *  
 * Parameters:
 *    type: Index in COMMAND_TABLE
 *    param: Parameters to the command (NULL if there are not any)
 *    irc_nick: Nickname of user that initiated the command
 *    target: Target channel (target is ALWAYS a channel)
 *
 * Return:
 *    Pointer to new Command
 */
static struct Command *
command_create(const struct OperCommandHash *tab, char *param, char *irc_nick,
               const struct ChannelConf *target)
{
  struct Command *const ret = xcalloc(sizeof *ret);

  if (param)
    ret->param = xstrdup(param);

  ret->tab = tab;
  ret->irc_nick = xstrdup(irc_nick);
  ret->target = target;  /* FIXME: This needs fixed if rehash is implemented */

  time(&ret->added);

  return ret;
}

/* command_free
 * 
 *   Free a command struct
 *
 * Parameters:
 *   command: Command struct to free
 *   
 * Return: NONE
 */
static void
command_free(struct Command *command)
{
  if (command->param)
    xfree(command->param);

  xfree(command->irc_nick);
  xfree(command);
}

/* command_userhost
 *
 *    A 302 reply was received. The reply is parsed to check if the
 *    user was an operator. If so any commands they had queued are
 *    executed.
 *  
 * Parameters:
 *    reply: Reply to USERHOST    (ex: :grifferz*=+goats@pc-62-30-219-54-pb.blueyonder.co.uk)
 *
 * Return: NONE
 * 
 */
void
command_userhost(const char *reply)
{
  node_t *node, *node_next;
  char *tmp;
  int oper = 0;

  tmp = strchr(reply, '=');

  /* They quit, ignore it */
  if (tmp == NULL)
    return;

  /* Operators have a * flag in a USERHOST reply */
  if (*(tmp - 1) == '*')
    oper = 1;

  /* Null terminate it so tmp = the oper's nick */
  if (oper)
    *(--tmp) = '\0';
  else
    *(tmp) = '\0';

  /* Find any queued commands that match this user */
  LIST_FOREACH_SAFE(node, node_next, COMMANDS->head)
  {
    struct Command *cs = node->data;

    if (strcmp(cs->irc_nick, reply) == 0)
    {
      if (oper)
        cs->tab->handler(cs->param, cs->target);

      /* Cleanup the command */
      command_free(cs);
      list_remove(COMMANDS, node);
      node_free(node);
    }
  }
}

/* cmd_check
 *
 *    Start a manual scan on given IP address/hostname.
 *
 * Parameters:
 *    param: Parameters of the command
 *    target: channel command was sent to
 *
 */
static void
cmd_check(char *param, const struct ChannelConf *target)
{
  scan_manual(param, target);
}

/* cmd_stat
 *
 *   Send output of stats to channel.
 *
 * Parameters:
 *    param: Parameters of the command
 *    target: channel command was sent to
 */
static void
cmd_stat(char *param, const struct ChannelConf *target)
{
  stats_output(target->name);
}

/* cmd_fdstat
 *
 *   Send output of stats to channel.
 *
 * Parameters:
 *    param: Parameters of the command
 *    target: channel command was sent to
 */
static void
cmd_fdstat(char *param, const struct ChannelConf *target)
{
  fdstats_output(target->name);
}
