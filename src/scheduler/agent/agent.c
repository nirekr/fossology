/* **************************************************************
Copyright (C) 2010 Hewlett-Packard Development Company, L.P.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 ************************************************************** */

/*
 * TODO change agent logging to only include JOB[job_id].name[pid] only when usefull
 * TODO change the "<date> <time> scheduler ::" to "<date> <time> agent ::" for some
 *      log messages
 */

/* local includes */
#include <agent.h>
#include <database.h>
#include <event.h>
#include <host.h>
#include <job.h>
#include <logging.h>
#include <scheduler.h>

/* library includes */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* unix library includes */
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* other library includes */
#include <glib.h>

/* agent defines */
#define MAX_ARGS 32       ///< the maximum number arguments passed to children  (arbitrary)
#define TILL_DEATH 180    ///< how long to wait before agent is dead            (3 minutes)
#define NUM_UPDATES 5     ///< the number of updates before agent is dead       (arbitrary)

#define TEST_NULV(a) if(!a) { errno = EINVAL; ERROR("agent passed is NULL, cannot proceed"); return; }
#define TEST_NULL(a, ret) if(!a) { errno = EINVAL; ERROR("agent passed is NULL, cannot proceed"); return ret; }

GTree* meta_agents = NULL;   ///< The master list of all meta agents
GTree* agents      = NULL;   ///< The master list of all of the agents

/* ************************************************************************** */
/* **** Data Types ********************************************************** */
/* ************************************************************************** */

/**
 * strings used to print the status of an agent. The status is actually an enum,
 * which is just an integer, so this provides the status in a human readable
 * format.
 */
const char* agent_status_strings[] = {
    "AG_FAILED",
    "AG_CREATED",
    "AG_SPAWNED",
    "AG_RUNNING",
    "AG_PAUSED",
    "AG_CLOSED"};

/* ************************************************************************** */
/* **** Local Functions ***************************************************** */
/* ************************************************************************** */

/**
 * Changes the status of the agent internal to the scheduler. This function
 * is used to transition between agent states instead of a raw set of the status
 * so that correct printing of the verbose message is guaranteed
 *
 * @param a the agent to change the status for
 * @param new_status the new status of the agentchar* sysconfdir = NULL;    // system configuration directory (SYSCONFDIR)
 */
void agent_transition(agent a, agent_status new_status)
{
  if(TVERBOSE3)
    clprintf("JOB[%d].%s[%d]: agent status changed: %s -> %s\n",
        job_id(a->owner), a->meta_data->name, a->pid,
        agent_status_strings[a->status], agent_status_strings[new_status]);
  a->status = new_status;
}

/**
 * Fails an agent. This will move the agent status to AG_FAILED and send a
 * SIGKILL to the relevant agent. It will also update the agents status within
 * the job that owns it and close the associated communication thread.
 *
 * @param a the agent that is failing.
 */
void agent_fail(agent a)
{
  TEST_NULV(a);
  agent_transition(a, AG_FAILED);
  job_fail_agent(a->owner, a);
  if(write(a->to_parent, "@@@1\n", 5) != 5)
  {
    ERROR("JOB[%d].%s[%d]: Failed to kill agent thread cleanly",
        job_id(a->owner), a->meta_data->name, a->pid);
  }
}

/**
 * This function will be called by g_tree_foreach() which is the reason for its
 * formatting. This will close all of the agent's pipes
 *
 * @param pid_ptr the key that was used to store this agent
 * @param a the agent that is being closed
 * @param excepted this is an agent we don't want to close, this is it
 * @return always returns 0 to indicate that the traversal should continue
 */
int agent_close_fd(int* pid_ptr, agent a, agent excepted)
{
  TEST_NULL(a, 0);
  if(a != excepted)
  {
    close(a->from_child);
    close(a->to_child);
    fclose(a->read);
    fclose(a->write);
  }
  return 0;
}

/**
 * Check the status and check in time of an agent.
 *   - if we haven't gotten a recent communication, close it
 *   - if it hasn't been performing tasks, close it
 *
 * @param pid_ptr pointer to key in g_tree, is not used in this function
 * @param a the agent that needs to be updated
 * @param unused data that is also not used in this function
 * @return always returns 0 to indicate that the traversal should continue
 */
int update(int* pid_ptr, agent a, gpointer unused)
{
  TEST_NULL(a, 0);
  if(a->status == AG_SPAWNED || a->status == AG_RUNNING || a->status == AG_PAUSED)
  {
    /* check last checkin time */
    if(time(NULL) - a->check_in > TILL_DEATH && !job_is_paused(a->owner))
    {
      ERROR("JOB[%d].%s[%d] no heartbeat for %d seconds",
          job_id(a->owner), job_type(a->owner), a->pid, time(NULL) - a->check_in);
      kill(a->pid, SIGKILL);
      return 0;
    }

    /* check items processed */
    if(a->status != AG_PAUSED && a->check_analyzed == a->total_analyzed)
      a->n_updates++;
    else
      a->n_updates = 0;
    if(a->n_updates > NUM_UPDATES)
    {
      kill(a->pid, SIGKILL);
      return 0;
    }

    a->check_analyzed = a->total_analyzed;

    VERBOSE3("JOB[%d].%s[%d]: agent updated correctly, processed %d items\n",
        job_id(a->owner), a->meta_data->name, a->pid, a->check_analyzed);
  }

  return 0;
}

/**
 * GTraversalFunction that kills all of the agents. This is used for an unclean
 * death since all of the child processes will be sent a kill signal instead of
 * existing cleanly.
 *
 * @param pid the process id associated with the agent
 * @param a pointer to the information associated with an agent
 * @param unused
 * @return always returns 0 to indicate that the traversal should continue
 */
int agent_kill(int* pid, agent a, gpointer unused)
{
  kill(a->pid, SIGKILL);
  return 0;
}

/**
 * GTraverseFunction that will print he name of every agent in alphabetical
 * order separated by spaces.
 *
 * @param name the name of the agent
 * @param ma the meta_agetns structure associated with the specific name
 * @param ostr the output stream to write the data to, socket in this case
 * @return always returns 0 to indicate that the traversal should continue
 */
int agent_list(char* name, meta_agent ma, GOutputStream* ostr)
{
  g_output_stream_write(ostr, name, strlen(name), NULL, NULL);
  g_output_stream_write(ostr, " ",  1,            NULL, NULL);
  return 0;
}

/**
 * GTraversalFunction that will test all of the agents. This will create
 * a job and an agent for each type of agent and traverses the meta_agent
 * tree instead of the agent tree.
 *
 * @param name the name of the meta agent (e.g. "nomos", "copyright", etc...)
 * @param ma the meta_agent structure needed for agent creation
 * @param h the host to start the agent on
 * @return always returns 0 to indicate that the traversal should continue
 */
int agent_test(char* name, meta_agent ma, host h)
{
  static int id_gen = -1;

  VERBOSE3("META_AGENT[%s] testing\n", ma->name);
  job j = job_init(ma->name, id_gen--, 0);
  agent_init(h, j);
  return 0;
}

/**
 * Listens for information from the agent. Starts by waiting for the agent to
 * send SPAWNED, then it will wait for any other information from the agent.
 * Information that it can receive includes:
 *
 * TODO list things that can be received
 *
 * @param a the agent that will be listened on
 */
void agent_listen(agent a)
{
  /* status locals */
  static GStaticMutex version_lock = G_STATIC_MUTEX_INIT;

  /* locals */
  char buffer[1024];          // buffer to store c strings read from agent, size is arbitraryssed
  int i;                      // simple indexing variable

  TEST_NULV(a);


  /* validate the agent version */
  if(fgets(buffer, sizeof(buffer), a->read) == NULL)
  {
    alprintf(job_log(a->owner),
        "T_FATAL %s.%d: JOB[%d].%s[%d] pipe from child closed\nT_FATAL errno is: %s\n",
        __FILE__, __LINE__, job_id(a->owner), a->meta_data->name, a->pid, strerror(errno));
    g_thread_exit(NULL);
  }

  buffer[strlen(buffer) - 1] = '\0';
  if(strncmp(buffer, "VERSION: ", 9) != 0)
  {
    alprintf(job_log(a->owner),
        "T_FATAL %s.%d: JOB[%d].%s[%d] agent didn't send version information\n",
        __FILE__, __LINE__, job_id(a->owner), a->meta_data->name, a->pid);
    alprintf(job_log(a->owner),
        "T_FATAL: received \"%s\" and expecting \"VERSION: <version>\"\n",
        buffer);
    clprintf("ERROR %s.%d: agent %s has been invalidated, removing from agents\n",
        __FILE__, __LINE__, a->meta_data->name);
    if(job_id(a->owner) >= 0)
      g_tree_remove(meta_agents, a->meta_data->name);
    g_thread_exit(NULL);
  }

  g_static_mutex_lock(&version_lock);
  strcpy(buffer, &buffer[9]);
  if(a->meta_data->version == NULL && a->meta_data->valid)
  {
    a->meta_data->version = g_strdup(buffer);
    if(TVERBOSE2)
      clprintf("META_AGENT[%s] version is: \"%s\"\n",
          a->meta_data->name, a->meta_data->version);
  }
  else if(strcmp(a->meta_data->version, buffer) != 0)
  {
    alprintf(job_log(a->owner),
        "ERROR %s.%d: META_DATA[%s] invalid agent spawn check\n",
        __FILE__, __LINE__, a->meta_data->name);
    alprintf(job_log(a->owner),
        "ERROR: version don't match: \"%s\" != received: \"%s\"\n",
        a->meta_data->version, buffer);
    a->meta_data->valid = 0;
    kill(a->pid, SIGKILL);
    g_static_mutex_unlock(&version_lock);
    return;
  }
  g_static_mutex_unlock(&version_lock);

  /* enter listening loop */
  while(1)
  {
    /* get message from agent */
    if(fgets(buffer, sizeof(buffer), a->read) == NULL)
      g_thread_exit(NULL);

    buffer[strlen(buffer) - 1] = '\0';

    if(strlen(buffer) == 0)
      continue;

    if(TVERBOSE3)
      alprintf(job_log(a->owner),
          "JOB[%d].%s[%d]: received: \"%s\"\n",
          job_id(a->owner), a->meta_data->name, a->pid, buffer);

    /* check for messages from scheduler or clean agent death */
    if(strncmp(buffer, "BYE", 3) == 0)
    {
      if((a->return_code = atoi(&(buffer[4]))) != 0)
      {
        alprintf(job_log(a->owner),
            "JOB[%d].%s[%d]: agent failed with error code %d\n",
            job_id(a->owner), a->meta_data->name, a->pid, a->return_code);
        agent_fail(a);
      }
      break;
    }

    if(strncmp(buffer, "@@@1", 4) == 0)
    {
      break;
    }
    if(strncmp(buffer, "@@@0", 4) == 0 && a->updated)
    {
      aprintf(a, "%s\n", a->data);
      aprintf(a, "END\n");
      fflush(a->write);
      a->updated = 0;
      continue;
    }

    /* if we get here it is an actual message from the agent */
    a->check_in = time(NULL);

    /* the agent has indicated that it is ready for data */
    if(strncmp(buffer, "OK", 2) == 0)
    {
      event_signal(agent_ready_event, a);
    }
    /* heart beat received from agent */
    else if(strncmp(buffer, "HEART", 5) == 0)
    {
      a->n_updates++;
      i = atoi(&buffer[7]);
      a->check_analyzed = i - a->total_analyzed;
      a->total_analyzed = i;
      database_job_processed(job_id(a->owner), a->total_analyzed);
    }
    else if(strncmp(buffer, "EMAIL", 5) == 0)
    {
      job_set_message(a->owner, g_strdup(buffer + 6));
    }
    /* we aren't quite sure what the agent sent, log it */
    else if(!(TVERBOSE3))
    {
      alprintf(job_log(a->owner),
          "JOB[%d].%s[%d]: \"%s\"\n",
          job_id(a->owner), a->meta_data->name, a->pid, buffer);
    }
  }

  if(TVERBOSE3)
  {
    alprintf(job_log(a->owner),
        "JOB[%d].%s[%d]: communication thread closing\n",
        job_id(a->owner), a->meta_data->name, a->pid);
  }
}

/**
 * TODO
 *
 * @param input
 * @param argc
 * @param argv
 */
void shell_parse(char* input, int* argc, char*** argv)
{
  char* begin;
  char* curr;
  int idx = 0;

  *argv = g_new0(char*, 30);
  begin = NULL;

  for(curr = input; *curr; curr++)
  {
    if(*curr == ' ')
    {
      if(begin == NULL)
        continue;

      if(*begin == '"')
        continue;

      *curr = '\0';
      (*argv)[idx++] = g_strdup(begin);
      begin = NULL;
    }
    else if(begin == NULL)
    {
      begin = curr;
    }
    else if(*begin == '"' && *curr == '"')
    {
      *begin = '\0';
      *curr  = '\0';

      (*argv)[idx++] = g_strdup(begin + 1);
      begin = NULL;
    }
  }

  (*argv)[idx++] = "-c";
  (*argv)[idx++] = sysconfigdir;
  (*argv)[idx++] = "--scheduler_start";
  (*argc) = idx;
}

/**
 * Spawns a new agent using the command passed in using the meta agent. This
 * function will call the fork and exec necessary to create a new agent. As a
 * result what this function does will change depending on if it is running in
 * the child or the parent.
 *
 * child:
 *   will duplicate the stdin, stdout, and stderr pipes for printing to the
 *   scheduler, parse the command line options for the agent and start the
 *   agent. It will then call exec to start the new agent process
 *
 * parent:
 *   this will enter the listen function, and wait for information from the
 *   child, either as a failure or as an update for the information being
 *   analyzed
 *
 * @param passed a pointer to the agent that is being spawned
 */
void* agent_spawn(void* passed)
{
  /* locals */
  agent a = (agent)passed;    // the agent that is being spawned
  gchar* tmp;                 // pointer to temporary string
  gchar** args;               // the arguments that will be passed to the child
  int argc;                   // the number of arguments parsed
  char buffer[2048];          // character buffer

  TEST_NULL(a, NULL);
  /* we are in the child */
  if((a->pid = fork()) == 0)
  {
    /* set the child's stdin and stdout to use the pipes */
    dup2(a->from_parent, fileno(stdin));
    dup2(a->to_parent, fileno(stdout));
    dup2(a->to_parent, fileno(stderr));

    /* close all the unnecessary file descriptors */
    g_tree_foreach(agents, (GTraverseFunc)agent_close_fd, a);
    close(a->from_child);
    close(a->to_child);

    /* set the priority of the process to the job's priority */
    if(nice(job_priority(a->owner)) == -1)
      ERROR("unable to correctly set priority of agent process %d", a->pid);

    /* if host is null, the agent will run locally to */
    /* run the agent localy, use the commands that    */
    /* were parsed when the meta_agent was created    */
    if(strcmp(host_address(a->host_machine), "localhost") == 0)
    {
      shell_parse(a->meta_data->raw_cmd, &argc, &args);

      tmp = args[0];
      args[0] = g_strdup_printf(AGENT_BINARY,
          sysconfigdir,
          a->meta_data->name,
          tmp);

      strcpy(buffer, args[0]);
      *strrchr(buffer, '/') = '\0';
      if(chdir(buffer) != 0) {
        ERROR("unable to change working directory: %s\n", strerror(errno));
      }

      execv(args[0], args);
    }
    /* otherwise the agent willprintf("HELLO\n");l be started using ssh   */
    /* if the agent is started using ssh we don't need */
    /* to fully parse the arguments, just pass the run */
    /* command as the last argument to the ssh command */
    else
    {
      args = g_new0(char*, 4);
      sprintf(buffer, AGENT_BINARY,
          host_agent_dir(a->host_machine),
          a->meta_data->name,
          a->meta_data->raw_cmd);
      args[0] = "/usr/bin/ssh";
      args[1] = host_address(a->host_machine);
      args[2] = buffer;
      args[3] = 0;
      execv(args[0], args);
    }

    /* we should never reach here */
    lprintf("ERROR %s.%d: JOB[%d].%s[%d] exec failed\nERROR errno is: %s\n",
        __FILE__, __LINE__, job_id(a->owner), a->meta_data->name, getpid(), strerror(errno));
  }
  /* we are in the parent */
  else if(a->pid > 0)
  {
    event_signal(agent_create_event, a);
    agent_listen(a);
  }
  /* error case */
  else
  {
    clprintf("ERROR %s.%d: JOB[%d].%s[%d] fork failed\nERROR errno is: %s\n",
        __FILE__, __LINE__, job_id(a->owner), a->meta_data->name, getpid(), strerror(errno));
  }

  return NULL;
}

/* ************************************************************************** */
/* **** Constructor Destructor ********************************************** */
/* ************************************************************************** */

/**
    a->generation = 0;
 * Creates a new meta agent. This will take and parse the information necessary
 * for the creation of a new agent instance. The name of the agent, the cmd for
 * starting the agent, the number of these agents that can run simutaniously,
 * and any special conditions for this agent. This function is where the cmd
 * will get parsed to be passed as command line args to the new agent.
 *
 * @param name the name of the agent (i.e. nomos, buckets, etc...)
 * @param cmd the command for starting the agent in a shell
 *
 * @param max the number of these that can concurrently, -1 for no limit
 * @param spc any special conditions associated with the agent
 * @return
 */
meta_agent meta_agent_init(char* name, char* cmd, int max, int spc)
{
  /* locals */
  meta_agent ma;
  char cpy[MAX_CMD + 1];

  /* test inputs */
  if(!name || !cmd)
  {
    ERROR("invalid arguments passed to meta_agent_init()");
    return NULL;
  }

  /* confirm valid inputs */
  if(strlen(name) > MAX_NAME || strlen(cmd) > MAX_CMD)
  {
    lprintf("ERROR failed to load %s meta agent", name);
    return NULL;
  }

  /* inputs are valid, create the meta_agent */
  ma = g_new0(struct meta_agent_internal, 1);

  strcpy(cpy, cmd);
  strcpy(ma->name, name);
  strcpy(ma->raw_cmd, cmd);
  strcat(ma->raw_cmd, " --scheduler_start");
  ma->max_run = max;
  ma->special = spc;
  ma->version = NULL;
  ma->valid = 1;

  return ma;
}

/**
 * Free the memory associated with a meta_agent. This is a destructor, and as a
 * result the meta_agent should not be used again after a call to this method
 *
 * @param ma the meta_agent to clear
 */
void meta_agent_destroy(meta_agent ma)
{
  TEST_NULV(ma);
  g_free(ma->version);
  g_free(ma);
}

/**
 * allocate and spawn a new agent. The agent that is spawned will be of the same
 * type as the meta_agent that is passed to this function and the agent will run
 * on the host that is passed.
 *
 * @param host_machine the machine to start the agent on
 * @param owner the job that this agent belongs to
 * @param gen the generation of the data associated with this agent
 */
agent agent_init(host host_machine, job owner)
{
  /* local variables */
  agent a;
  int child_to_parent[2];
  int parent_to_child[2];

  /* check job input */
  if(!owner)
  {
    lprintf("ERROR %s.%d: NULL job passed to agent init\n", __FILE__, __LINE__);
    lprintf("ERROR: no other information available\n");
    return NULL;
  }

  /* check that the agent type exists */
  if(g_tree_lookup(meta_agents, job_type(owner)) == NULL)
  {
    lprintf("ERROR %s.%d: jq_pk %d jq_type %s does not match any module in mods-enabled\n",
        __FILE__, __LINE__, job_id(owner), job_type(owner));
    job_fail(owner);
    job_remove_agent(owner, NULL);
    return NULL;
  }

  /* allocate memory and do trivial assignments */
  a = g_new(struct agent_internal, 1);
  a->meta_data = g_tree_lookup(meta_agents, job_type(owner));
  a->status = AG_CREATED;

  /* make sure that there is a metaagent for the job */
  if(a->meta_data == NULL)
  {
    ERROR("meta agent %s does not exist", job_type(owner));
    return NULL;
  }

  /* check if the agent is valid */
  if(!a->meta_data->valid)
  {
    ERROR("agent %s has been invalidated by version information", job_type(owner));
    return NULL;
  }

  /* create the pipes between the child and the parent */
  if(pipe(parent_to_child) != 0)
  {
    ERROR("JOB[%d.%s] failed to create parent to child pipe", job_id(owner), job_type(owner));
    return NULL;
  }
  if(pipe(child_to_parent) != 0)
  {
    ERROR("JOB[%d.%s] failed to create child to parent pipe", job_id(owner), job_type(owner));
    return NULL;
  }

  /* set file identifiers to correctly talk to children */
  a->from_parent = parent_to_child[0];
  a->to_child = parent_to_child[1];
  a->from_child = child_to_parent[0];
  a->to_parent = child_to_parent[1];

  /* initialize other info */
  a->host_machine = host_machine;
  a->owner = owner;
  a->updated = 0;
  a->n_updates = 0;
  a->data = NULL;
  a->return_code = -1;

  /* open the relevant file pointers */
  if((a->read = fdopen(a->from_child, "r")) == NULL)
  {
    ERROR("JOB[%d.%s] failed to initialize read file", job_id(owner), job_type(owner));
    return NULL;
  }
  if((a->write = fdopen(a->to_child, "w")) == NULL)
  {
    ERROR("JOB[%d.%s] failed to initialize write file", job_id(owner), job_type(owner));
    return NULL;
  }

  /* spawn the listen thread */
  a->thread = g_thread_create(agent_spawn, a, 1, NULL);
  return a;
}

/**
 * frees the memory associated with an agent.
 *
 * This include:
 *  1. all of the files that are open in the agent
 *  2. all of the pipes still open for the agent
 *  3. inform the os that the process can die using a waitpid()
 *  4. free the internal data structure of the agent
 *
 * @param a the agent to destroy
 */
void agent_destroy(agent a)
{
  TEST_NULV(a);

  /* close all of the files still open for this agent */
  close(a->from_child);
  close(a->to_child);
  close(a->from_parent);
  close(a->to_parent);
  fclose(a->write);
  fclose(a->read);

  /* release the child process */
  g_free(a);
}

/* ************************************************************************** */
/* **** Modifier Functions ************************************************** */
/* ************************************************************************** */

/**
 * Event created when a SIGCHLD is received for an agent. If one SIGCHILD is
 * received for several process deaths, there will be seperate events for each
 * pid.
 *
 * @param pid the pid of the process that died
 */
void agent_death_event(pid_t* pid)
{
  agent a;

  if((a = g_tree_lookup(agents, pid)) == NULL)
    return;

  if(job_id(a->owner) >= 0)
    event_signal(database_update_event, NULL);

  if(write(a->to_parent, "@@@1\n", 5) != 5)
    VERBOSE2("JOB[%d].%s[%d]: write to agent unsuccessful: %s\n",
        job_id(a->owner), a->meta_data->name, a->pid, strerror(errno));
  g_thread_join(a->thread);

  if(a->return_code != 0)
  {
    alprintf(job_log(a->owner), "JOB[%d].%s[%d]: agent failed\n",
        job_id(a->owner), a->meta_data->name, a->pid);
    ERROR("JOB[%d].%s[%d]: agent closed unexpectedly, agent status was %s",
        job_id(a->owner), a->meta_data->name, a->pid, agent_status_strings[a->status]);
    agent_fail(a);
  }

  if(a->status != AG_PAUSED && a->status != AG_FAILED)
    agent_transition(a, AG_PAUSED);

  VERBOSE2("JOB[%d].%s[%d]: successfully removed from the system\n",
      job_id(a->owner), a->meta_data->name, a->pid);

  job_update(a->owner);
  job_remove_agent(a->owner, a);
  if(a->status == AG_FAILED && job_id(a->owner) < 0)
  {
    lprintf("ERROR %s.%d: agent %s has failed scheduler startup test\n",
        __FILE__, __LINE__, a->meta_data->name);
    g_tree_remove(meta_agents, a->meta_data->name);
  }

  g_tree_remove(agents, &a->pid);
  g_free(pid);
}

/**
 * Event created when a new agent has been created. This means that the agent
 * has been allocated internally and the fork() call has successfully executed.
 * The agent has not yet communicated with the scheduler when this event is
 * created.
 *
 * @param a the agent that has been created.
 */
void agent_create_event(agent a)
{
  TEST_NULV(a);

  VERBOSE3("JOB[%d].%s[%d]: agent successfully spawned\n",
      job_id(a->owner), a->meta_data->name, a->pid);

  g_tree_insert(agents, &a->pid, a);
  agent_transition(a, AG_SPAWNED);
  job_add_agent(a->owner, a);
}

/**
 * Event created when an agent is ready for more data. This will event will be
 * created when an agent first communicates with the scheduler, so this will
 * handle changing its status to AG_RUNNING. This will also be created every
 * time an agent finishes a block of data.
 *
 * @param a the agent that is ready.
 */
void agent_ready_event(agent a)
{
  TEST_NULV(a);
  if(a->status == AG_SPAWNED)
  {
    agent_transition(a, AG_RUNNING);
    VERBOSE2("JOB[%d].%s[%d]: agent successfully created\n",
        job_id(a->owner), a->meta_data->name, a->pid);
  }

  if(!job_is_open(a->owner))
  {
    agent_transition(a, AG_PAUSED);
    job_finish_agent(a->owner, a);
    job_update(a->owner);
    return;
  }
  else
  {
    a->data = job_next(a->owner);
    a->updated = 1;
  }

  if(write(a->to_parent, "@@@0\n", 5) != 5)
  {
    ERROR("JOB[%d].%s[%d]: failed sending new data to agent",
        job_id(a->owner), a->meta_data->name, a->pid);
    kill(a->pid, SIGKILL);
  }
}

/**
 * Event created when the scheduler receives a SIGALRM. This will loop over
 * every agent and call the update function on it. This will kill any agents
 * that are hung without heart beat or any agents that have stopped updating
 * the number of item processed.
 *
 * @param unused needed since this an event, but should be NULL
 */
void agent_update_event(void* unused)
{
  g_tree_foreach(agents, (GTraverseFunc)update, NULL);
}

/**
 * Pauses an agent, this will pause the agent by sending a SIGSTOP to the
 * process and then decrease the load on the host machine.
 *
 * @param a the agent to pause
 */
void agent_pause(agent a)
{
  kill(a->pid, SIGSTOP);
  host_decrease_load(a->host_machine);
}

/**
 * Unpause the agent, this will send a SIGCONT to the process regardless of if
 * a SIGTOP was sent. If the process wasn't SIGSTOP'd this will do nothing. Also
 * increases the load on the host.
 *
 * @param a the agent to unpause
 */
void agent_unpause(agent a)
{
  kill(a->pid, SIGCONT);
  host_increase_load(a->host_machine);
}

/**
 * Prints the status of the agent to the output stream provided. The formating
 * for this is as such:
 *   agent:<pid> host:<host> type:<type> status:<status> time:<time>
 *
 * @param a
 * @param ostr
 */
void agent_print_status(agent a, GOutputStream* ostr)
{
  gchar* status_str;
  char time_buf[64];
  struct tm* time_info;

  TEST_NULV(a);
  TEST_NULV(ostr);

  strcpy(time_buf, "(none)");
  time_info = localtime(&a->check_in);
  if(time_info)
    strftime(time_buf, sizeof(time_buf), "%F %T", localtime(&a->check_in));
  status_str = g_strdup_printf("agent:%d host:%s type:%s status:%s time:%s\n",
      a->pid,
      host_name(a->host_machine),
      a->meta_data->name,
      agent_status_strings[a->status],
      time_buf);

  VERBOSE2("AGENT_STATUS: %s", status_str);
  g_output_stream_write(ostr, status_str, strlen(status_str), NULL, NULL);
  g_free(status_str);
  return;
}

/**
 * Gets the status field for the agent
 *
 * @param a an agent
 * @return the status
 */
agent_status agent_gstatus(agent a)
{
  return a->status;
}

/**
 * Acts as a standard printf, but prints the agents instead of stdout. This is
 * the main function used by the scheduler when communicating with the agents.
 *
 * @param a the agent to send the formated data to
 * @param fmt the formating string for the data
 * @return if the print was successful
 */
int aprintf(agent a, const char* fmt, ...)
{
  va_list args;
  int rc;
  char* tmp;

  va_start(args, fmt);
  if(TVERBOSE3)
  {
    tmp = g_strdup_vprintf(fmt, args);
    tmp[strlen(tmp) - 1] = '\0';
    alprintf(job_log(a->owner), "JOB[%d].%s[%d]: sent to agent \"%s\"\n",
        job_id(a->owner), a->meta_data->name, a->pid, tmp);
    rc = fprintf(a->write, "%s\n", tmp);
    g_free(tmp);
  }
  else
  {
    rc = vfprintf(a->write, fmt, args);
  }
  va_end(args);
  fflush(a->write);

  return rc;
}

/**
 * Gets the pid of the process associated with this agent
 *
 * @param a relevant agent
 * @return the pid of the process
 */
int agent_pid(agent a)
{
  return a->pid;
}

/**
 * Write information to the communication thread for the agent. This is used
 * when the scheduler needs to wake up or kill the thread used to talk to the
 * agent. When using this function, one should always print "@@@..." where ...
 * is the message that is actually getting sent.
 *
 * @param a the agent to send the information to
 * @param buf the actual data
 * @param count the number of bytes to write to the agent
 * @return returns if the write was successful
 */
ssize_t agent_write(agent a, const void* buf, int count)
{
  return write(a->to_parent, buf, count);
}

/* ************************************************************************** */
/* **** static functions and meta agents ************************************ */
/* ************************************************************************** */

/**
 * Calls the agent test function for every type of agent. This is used when
 * either the -t or -T option are used upon scheduler creation.
 *
 * @param h the host to start the agents on,
 */
void test_agents(host h)
{
  g_tree_foreach(meta_agents, (GTraverseFunc)agent_test, h);
}

/**
 * Call the agent_kill function for every agent within the system. This will
 * send a SIGKILL to every child process of the scheduler. Used when shutting
 * down the scheduler.
 */
void kill_agents()
{
  g_tree_foreach(agents, (GTraverseFunc)agent_kill, NULL);
}

/**
 * TODO
 *
 * @param ostr
 */
void list_agents(GOutputStream* ostr)
{
  g_tree_foreach(meta_agents, (GTraverseFunc)agent_list, ostr);
  g_output_stream_write(ostr, "\nend\n", 4, NULL, NULL);
}

/**
 * Will create the meta_agents and agents maps
 */
void agent_list_init(void) {
  meta_agents = g_tree_new_full(string_compare, NULL, NULL, (GDestroyNotify)meta_agent_destroy);
  agents      = g_tree_new_full(int_compare   , NULL, NULL, (GDestroyNotify)agent_destroy);
}

/**
 * destroys both the meta agent and agent lists. This is used when the scheduler
 * is cleanly shutting down or when the scheduler is reloading its configuration
 * data.
 */
void agent_list_clean()
{
  if(meta_agents) g_tree_destroy(meta_agents);
  if(agents) g_tree_destroy(agents);
  agent_list_init();
}

/**
 * Creates a new meta agent and adds it to the list of meta agents. This will
 * parse the shell command that will start the agent process.
 *
 * @param name the name of the meta agent (e.g. "nomos", "copyright", etc...)
 * @param cmd the shell command used to the run the agent
 * @param max the max number of this type of agent that can run concurrently
 * @param spc anything special about the agent type
 */
int add_meta_agent(char* name, char* cmd, int max, int spc)
{
  meta_agent ma;

  if(!name || !cmd)
  {
    ERROR("couldn't add new meta agent");
    return 0;
  }

  if(meta_agents == NULL)
    agent_list_init();

  if(g_tree_lookup(meta_agents, name) == NULL)
  {
    ma = meta_agent_init(name, cmd, max, spc);
    g_tree_insert(meta_agents, ma->name, ma);
    return 1;
  }

  return 0;
}

/**
 * Checks to see if a particular agent type is available in the list of
 * meta agents.
 *
 * @param name the type of the agent (it's name)
 * @return if the type exists in the list of meta agents
 */
int is_meta_agent(char* name)
{
  return g_tree_lookup(meta_agents, name) != NULL;
}

/**
 * tests if a particular meta agent is exclusive.
 *
 * @param name the name of the meta agent
 * @return 1 if it is exclusive
 */
int is_exclusive(char* name)
{
  meta_agent ma = (meta_agent)g_tree_lookup(meta_agents, name);
  return ma == NULL || ma->special == SAG_EXCLUSIVE;
}

/**
 * Gets the number of agents that still exist within the scheduler. Not
 * all of these agents are necessarily associated with a child process, but
 * most should be.
 *
 * @return the number of agents in the agent tree
 */
int num_agents()
{
  return g_tree_nnodes(agents);
}
