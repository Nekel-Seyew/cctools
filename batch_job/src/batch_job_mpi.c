/*
Copyright (C) 2018- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
 */

#include "batch_job.h"
#include "batch_job_internal.h"
#include "debug.h"
#include "process.h"
#include "macros.h"
#include "stringtools.h"

#include "hash_table.h"
#include "jx.h"
#include "jx_parse.h"
#include "load_average.h"
#include <unistd.h>
#include "host_memory_info.h"
#include "int_sizes.h"
#include "itable.h"
#include "list.h"
#include "timestamp.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <assert.h>

#ifdef MPI

#include <mpi.h>

static struct hash_table* name_rank;
static struct hash_table* name_size;

static struct itable* rank_res;
static struct list* jobs;
static struct itable* rank_jobs;

static int gotten_resources = 0;

static int id = 1;

struct batch_job_mpi_workers_resources {
    long max_memory;
    int max_cores;
    long cur_memory;
    int cur_cores;
};

struct batch_job_mpi_job {
    int cores;
    int mem;
    int comp;
    char* cmd;
    int id;
    char* env;
    char* infiles;
    char* outfiles;
};

struct mpi_fit{
    int rank;
    double value;
};

static int sort_mpi_struct(void* a, void* b){
    struct mpi_fit* mfa = (struct mpi_fit*)a;
    struct mpi_fit* mfb = (struct mpi_fit*)b;
    return (mfa->value - mfb->value);
}

union mpi_ccl_guid {
    char c[8];
    unsigned int ul;
};

static unsigned int gen_guid() {
    FILE* ran = fopen("/dev/urandom", "r");
    if (!ran)
        fatal("Cannot open /dev/urandom");
    union mpi_ccl_guid guid;
    size_t k = fread(guid.c, sizeof (char), 8, ran);
    if (k < 8)
        fatal("couldn't read 8 bytes from /dev/urandom/");
    fclose(ran);
    return guid.ul;
}

void batch_job_mpi_give_ranks_sizes(struct hash_table* nr, struct hash_table* ns) {
    name_rank = nr;
    name_size = ns;
}

static void get_resources() {
    gotten_resources = 1;
    char* key;
    int value;
    char* worker_cmd;
    unsigned len;

    rank_res = itable_create(0);
    rank_jobs = itable_create(0);

    hash_table_firstkey(name_rank);
    while (hash_table_nextkey(name_rank, &key, (void**) &value)) {
        //ask for resources
        worker_cmd = string_format("{\"Orders\":\"Send-Resources\"}");
        len = strlen(worker_cmd);
        MPI_Send(&len, 1, MPI_UNSIGNED, value, 0, MPI_COMM_WORLD);
        MPI_Send(worker_cmd, len, MPI_CHAR, value, 0, MPI_COMM_WORLD);

        //recieve response
        debug(D_BATCH, "RANK0 Recieved %i resources\n", value);
        MPI_Recv(&len, 1, MPI_UNSIGNED, value, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        char* str = malloc(sizeof (char*)*len + 1);
        memset(str, '\0', sizeof (char)*len + 1);
        MPI_Recv(str, len, MPI_CHAR, value, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        //parse response
        struct jx* recobj = jx_parse_string(str);
        struct batch_job_mpi_workers_resources* res = malloc(sizeof (struct batch_job_mpi_workers_resources));
        res->cur_cores = res->max_cores = jx_lookup_integer(recobj, "cores");
        res->cur_memory = res->max_memory = jx_lookup_integer(recobj, "memory");
        itable_insert(rank_res, value, (void*) res);
        
        free(worker_cmd);
        free(str);
        jx_delete(recobj);
    }

    jobs = list_create();

}

static int find_fit(struct itable* comps, int req_cores, int req_mem, int job_id) {
    itable_firstkey(comps);
    UINT64_T* key;
    struct batch_job_mpi_workers_resources* comp;
    
    struct mpi_fit* blah = calloc(itable_size(comps),sizeof(struct mpi_fit)); //malloc(sizeof(struct mpi_fit) * itable_size(comps));
    int i = 0;
    while (itable_nextkey(comps, &key, (void**) &comp)) {
        //fprintf(stderr,"RANK0: examinig rank: %lu\n",key);
        //right now is first fit. Might want to try and find worst fit, aka, a fit on the LEAST busy resource
        if (comp->cur_cores - req_cores >= 0 && comp->cur_memory - req_mem >= 0) {
            
            blah[i].rank = key;
            blah[i].value = sqrt(pow((comp->cur_cores - req_cores) , 2) + pow((comp->cur_memory - req_mem),2));
            
            
//            struct batch_job_mpi_job* job_struct = malloc(sizeof (struct batch_job_mpi_job));
//            job_struct->cores = req_cores;
//            job_struct->mem = req_mem;
//            job_struct->comp = key;
//
//            comp->cur_cores -= req_cores;
//            comp->cur_memory -= req_mem;
//
//            itable_insert(rank_jobs, job_id, job_struct);
//
//            return key;
        }
        i +=1;
    }
    //fprintf(stderr,"RANK0 sorting fits\n");
    qsort((void*) blah, itable_size(comps), sizeof(struct mpi_fit), sort_mpi_struct);
    //for(i=0; i<itable_size(comps); i++){
    //    fprintf(stderr,"RANK0: Rank: %i value: %lf\n",blah[i].rank,blah[i].value);
    //}
    //fprintf(stderr,"RANK0: Last value: %lf\n",blah[i - 1].value);
    if (blah[itable_size(comps) - 1].value > 0.0) {
        int rank = blah[itable_size(comps) - 1].rank;
        //assert(rank != 0);
        //fprintf(stderr,"RANK0: found a good place for it, rank: %i!\n",rank);
        comp = itable_lookup(comps,rank);
        struct batch_job_mpi_job* job_struct = malloc(sizeof (struct batch_job_mpi_job));
        job_struct->cores = req_cores;
        job_struct->mem = req_mem;
        job_struct->comp = comp;

        //fprintf(stderr,"RANK0 updating worker resources: %p\n",(void*)comp);
        comp->cur_cores -= req_cores;
        comp->cur_memory -= req_mem;

        //fprintf(stderr,"RANK0: inserting into table!\n");
        itable_insert(rank_jobs, job_id, job_struct);
        //fprintf(stderr,"RANK0: returning the rank!\n");
        free(blah);
        return rank;
    }
    free(blah);
    return -1;
}

static batch_job_id_t batch_job_mpi_submit(struct batch_queue *q, const char *cmd, const char *extra_input_files, const char *extra_output_files, struct jx *envlist, const struct rmsummary *resources) {

    //some init stuff
    if (!gotten_resources) get_resources();

    int cores_req = resources->cores < 0 ? 1 : resources->cores;
    int mem_req = resources->memory < 0 ? 1000 : resources->memory;

    //push new job onto the list
    struct batch_job_mpi_job* job = malloc(sizeof (struct batch_job_mpi_job));
    job->cmd = xxstrdup(cmd);
    job->cores = cores_req;
    job->mem = mem_req;
    job->id = id++;
    job->env = envlist != NULL ? jx_print_string(envlist) : NULL;
    job->infiles = extra_input_files != NULL ? extra_input_files : "";
    job->outfiles = extra_output_files != NULL ? extra_output_files : "";
    struct batch_job_info *info = malloc(sizeof (*info));
    memset(info, 0, sizeof (*info));
    info->submitted = time(0);
    itable_insert(q->job_table, job->id, info);

    list_push_tail(jobs, job);
    
    debug(D_BATCH,"Queued job %i",job->id);

    return job->id;
}

static batch_job_id_t batch_job_mpi_wait(struct batch_queue * q, struct batch_job_info * info_out, time_t stoptime) {

    char* key;
    UINT64_T value;
    char* str;
    unsigned len;

    //first iterate through the jobs and see if we can fit ONE of them in the workers
    list_first_item(jobs);
    struct batch_job_mpi_job* job;
    while ((job = list_next_item(jobs)) != NULL) {
        int rank_fit = find_fit(rank_res, job->cores, job->mem, job->id);
        //debug(D_BATCH, "RANK0 Job %i needs %i cores %i mem\n", job->id, job->cores, job->mem);
        if (rank_fit < 0) {
            continue;
        }
        debug(D_BATCH, "RANK0 Job %i found a fit at %i. It needs %i cores %i mem\n", job->id, rank_fit, job->cores,job->mem);

        char* tmp = string_escape_shell(job->cmd);
        char* env = job->env != NULL ? job->env : "";
        char* worker_cmd = string_format("{\"Orders\":\"Execute\",\"CMD\":%s,\"ID\":%i,\"ENV\":%s,\"IN\":\"%s\",\"OUT\":\"%s\"}", tmp, job->id, env, job->infiles, job->outfiles);

        len = strlen(worker_cmd);
        MPI_Send(&len, 1, MPI_UNSIGNED, rank_fit, 0, MPI_COMM_WORLD);
        MPI_Send(worker_cmd, len, MPI_CHAR, rank_fit, 0, MPI_COMM_WORLD);
        debug(D_BATCH, "RANK0 Sent cmd object string to %i: %s\n", rank_fit, worker_cmd);
        debug(D_BATCH,"Job %i submitted to worker",job->id);
        list_remove(jobs, job);
        
        free(tmp);
        free(env);
        free(worker_cmd);
        break;
    }

    //See if we have a msg waiting for us using MPI_Iprobe
    //debug(D_BATCH,"RANK0 looking through our hash table for who has what!\n");
    if(!hash_table_nextkey(name_rank,&key,(void**)&value)) hash_table_firstkey(name_rank);
    while (hash_table_nextkey(name_rank, &key, (void**) &value)) {
        MPI_Status mstatus;
        int flag;
        MPI_Iprobe(value, 0, MPI_COMM_WORLD, &flag, &mstatus);
        if (flag) {
            fprintf(stderr, "RANK0 There is a job waiting to be returned from %i:%s\n", value, key);
            MPI_Recv(&len, 1, MPI_UNSIGNED, value, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            str = malloc(sizeof (char*)*len + 1);
            memset(str, '\0', sizeof (char)*len + 1);
            MPI_Recv(str, len, MPI_CHAR, value, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            struct jx* recobj = jx_parse_string(str);
            struct batch_job_info *info = info_out;
            memset(info, 0, sizeof (*info));
            info->started = jx_lookup_integer(recobj, "START");
            info->finished = jx_lookup_integer(recobj, "END");
            if ((info->exited_normally = jx_lookup_integer(recobj, "NORMAL")) == 1) {
                info->exit_code = jx_lookup_integer(recobj, "STATUS");
            } else {
                info->exit_signal = jx_lookup_integer(recobj, "SIGNAL");
            }
            int job_id = jx_lookup_integer(recobj, "ID");
            debug(D_BATCH,"Job %i returned",job_id);
            debug(D_BATCH,"Job %i started %i and was waited on at %i",job_id,info->started,info->finished);
            struct batch_job_info* old_info = itable_remove(q->job_table, job_id);
            info->submitted = old_info->submitted;
            itable_insert(q->job_table, jx_lookup_integer(recobj, "ID"), info);

            free(str);
            jx_delete(recobj);

            struct batch_job_mpi_job* jobstruct = itable_lookup(rank_jobs, job_id);
            itable_remove(rank_jobs, job_id);
            struct batch_job_mpi_workers_resources* comp = itable_lookup(rank_res, value);
            comp->cur_cores += jobstruct->cores;
            comp->cur_memory += jobstruct->mem;

            return job_id;

        }



    }
    //debug(D_BATCH,"RANK0: no jobs to be returned yet. moving on!\n");
    return -1;


}

static int batch_job_mpi_remove(struct batch_queue *q, batch_job_id_t jobid) {
    //tell a comp to kill this job
}

void batch_job_mpi_kill_workers() {
    char* key;
    int value;
    char* worker_cmd;
    hash_table_firstkey(name_rank);
    while (hash_table_nextkey(name_rank, &key, (void**) &value)) {
        worker_cmd = string_format("{\"Orders\":\"Terminate\"}");
        unsigned len = strlen(worker_cmd);
        MPI_Send(&len, 1, MPI_UNSIGNED, value, 0, MPI_COMM_WORLD);
        MPI_Send(worker_cmd, len, MPI_CHAR, value, 0, MPI_COMM_WORLD);
        free(worker_cmd);
    }
}

static int batch_queue_mpi_create(struct batch_queue *q) {
    batch_queue_set_feature(q, "mpi_job_queue", NULL);
    return 0;
}

static struct list* extract_file_names_from_list(char* in) {
    struct list* output = list_create();
    char* tmp = strdup(in);
    char* ta = strtok(tmp, ",");
    while (ta != NULL) {
        int push_success = list_push_tail(output, strdup(ta));
        if (!push_success) {
            fatal("Error appending file name to list due to being out of memory");
        }
        ta = strtok(0, ",");
    }

    return output;

}

int batch_job_mpi_worker_function(int worldsize, int rank, char* procname, int procnamelen) {

    if (worldsize < 2) {
        debug(D_BATCH, "Soemthing went terribly wrong.....");
    }

    //first, send name and rank to MPI master
    char* sendstr = string_format("{\"name\":\"%s\",\"rank\":%i}", procname, rank);
    unsigned len = strlen(sendstr);
    MPI_Send(&len, 1, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD);
    MPI_Send(sendstr, len, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
    free(sendstr);

    debug(D_BATCH, "%i Sent original msg to Rank 0!\n", rank);

    MPI_Recv(&len, 1, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    char* str = malloc(sizeof (char*)*len + 1);
    memset(str, '\0', sizeof (char)*len + 1);
    MPI_Recv(str, len, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    debug(D_BATCH, "%i recieved the live/die string from rank 0: %s\n", rank, str);

    struct jx* recobj = jx_parse_string(str);
    int cores;
    if ((cores = (int) jx_lookup_integer(recobj, "LIVE")) == 0) { //meaning we should die
        debug(D_BATCH, "%i:%s Being told to die, so doing that\n", rank, procname);
        MPI_Finalize();
        _exit(0);
    }
    int mem = (int) jx_lookup_integer(recobj, "MEM"); //since it returns 0 

    char* workdir = jx_lookup_string(recobj, "WORK_DIR") != NULL ? string_format("%s",jx_lookup_string(recobj, "WORK_DIR")) : NULL; //since NULL if not there
    char* cwd = get_current_dir_name();

    debug(D_BATCH, "%i has been told to live and how many cores we have. Creating itables and entering while loop\n", rank);

    struct itable* job_ids = itable_create(0);
    struct itable* job_times = itable_create(0);
    
    //cleanup before main loop
    jx_delete(recobj);
    free(str);

    while (1) {
        //might want to check MPI_Probe
        MPI_Status mstatus;
        int flag;
        MPI_Iprobe(0, 0, MPI_COMM_WORLD, &flag, &mstatus);
        if (flag) {
            debug(D_BATCH, "%i has orders msg from rank 0\n", rank);
            MPI_Recv(&len, 1, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            //debug(D_BATCH, "%i:%s len of new msg: %u\n", rank, procname, len);

            str = malloc(sizeof (char*)*len + 1);
            memset(str, '\0', sizeof (char)*len + 1);
            MPI_Recv(str, len, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);


            //debug(D_BATCH, "%i:%s parsing orders object\n", rank, procname);
            recobj = jx_parse_string(str);

            if (strstr(jx_lookup_string(recobj, "Orders"), "Terminate")) {
                //terminating, meaning we're done!
                debug(D_BATCH,"%i:%s Being told to terminate, calling finalize and returning\n");
                MPI_Finalize();
                return 0;
            }

            if (strstr(jx_lookup_string(recobj, "Orders"), "Send-Resources")) {
                debug(D_BATCH, "%i:%s sending resources to rank 0!\n", rank, procname);
                //need to send resources json object

                int cores_total = load_average_get_cpus();
                UINT64_T memtotal;
                UINT64_T memavail;
                host_memory_info_get(&memavail, &memtotal);
                int memory = ((memtotal / (1024 * 1024)) / cores_total) * cores; //MB
                //debug(D_BATCH, "%i:%s cores_total: %i cores: %i memtotal: %u mem_mine: %i\n", rank, procname, cores_total, cores, memtotal, mem);

                memory = mem != 0 ? mem : memory;
                sendstr = string_format("{\"cores\":%i,\"memory\":%i}", cores, memory);
                len = strlen(sendstr);
                MPI_Send(&len, 1, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD);
                MPI_Send(sendstr, len, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
                free(sendstr);
                //debug(D_BATCH, "%i:%s Done sending resources to Rank0 \n", rank, procname);

            }

            if (strstr(jx_lookup_string(recobj, "Orders"), "Execute")) {
                debug(D_BATCH, "%i:%s Executing given command!\n", rank, procname);
                char* cmd = jx_lookup_string(recobj, "CMD");
                int mid = jx_lookup_integer(recobj, "ID");
                struct jx* env = jx_lookup(recobj, "ENV");
                char* inf = jx_lookup_string(recobj, "IN");
                char* outf = jx_lookup_string(recobj, "OUT");
                debug(D_BATCH, "%i:%s The command to execute is: %s and it is job id %i\n", rank, procname, cmd, mid);
                //If workdir, make a sandbox.
                //see if input files outside sandbox, and need link in
                //if not, then copy over and link in.
                char* sandbox = NULL;
                if (workdir != NULL) {
		    debug(D_BATCH,"%i:%s workdir is not null, going to create sandbox! workdir pointer: %p workdir value: %s\n",rank,procname,(void*)workdir,workdir);
                    char* tmp;
                    sandbox = string_format("%s/%u", workdir, gen_guid());
                    tmp = string_format("mkdir %s", sandbox);
                    int kr = system(tmp);
                    if (kr != 0) debug(D_BATCH, "%i:%s tried to make sandbox %s: failed: %i\n", rank, procname, sandbox, kr);
                    free(tmp);
                    
                    char* tmp_ta = strdup(inf);
                    char* ta = strtok(tmp_ta, ",");
                    while (ta != NULL) {
                        tmp = string_format("%s/%s",sandbox,ta);
                        kr = link(ta,tmp);
                        free(tmp);
                        if(kr < 0){
                            tmp = string_format("cp -rf %s %s", ta, sandbox);
                            kr = system(tmp);
                            if (kr != 0) debug(D_BATCH, "%i:%s failed to copy %s to %s :: %i\n", rank, procname, ta, sandbox, kr);
                            free(tmp);
                        }
                        ta = strtok(0, ",");
                    }

                }
                int jobid = fork();
                if (jobid > 0) {
                    debug(D_BATCH, "%i:%s In the parent of the fork, the child id is: %i\n", rank, procname, jobid);
                    struct batch_job_info *info = malloc(sizeof (*info));
                    memset(info, 0, sizeof (*info));
                    info->started = time(0);
                    itable_insert(job_ids, jobid, mid);
                    itable_insert(job_times, mid, info);
                    //return jobid;
                } else if (jobid < 0) {
                    debug(D_BATCH, "%i:%s there was an error that prevented forking: %s\n", rank, procname, strerror(errno));
                    MPI_Finalize();
                    return -1;
                } else {
                    if (env) {
                        jx_export(env);
                    }
                    if (sandbox) {
                        //fprintf(stderr,"%i:%s-FORK:%i we are starting the cmd modification process\n",rank,procname,getpid());
                        char* tmp = string_format("cd %s && %s", sandbox, cmd);
                        cmd = tmp;
                        //need to cp from workdir to ./
                        char* tmp_da = strdup(outf);
                        char* ta = strtok(tmp_da, ",");
                        while (ta != NULL) {
                            tmp = string_format("%s && cp -rf ./%s %s/%s", cmd, ta, cwd, ta);
                            free(cmd);
                            cmd = tmp;
                            ta = strtok(0, ",");
                        }
                        tmp = string_format("%s && rm -rf %s", cmd, sandbox);
                        free(cmd);
                        cmd = tmp;
                    }
                    //fprintf(stderr,"%i:%s is starting child process with command: %s\n",rank,procname,cmd);
                    debug(D_BATCH, "%i:%s CHILD PROCESS:%i starting command!\n", rank, procname, getpid());
                    execlp("sh", "sh", "-c", cmd, (char *) 0);
                    _exit(127); // Failed to execute the cmd.
                }
            }
            jx_delete(recobj);
            free(str);
        }

        //before looping again, check for exit
        int status;
        int i = waitpid(0, &status, WNOHANG);
        if (i == 0) {
            continue;
        } else if (i == -1 && errno == ECHILD) {
            continue;
        }
        debug(D_BATCH, "%i:%s Child process %i has returned! looking it up and processing it\n", rank, procname, i);
        int k = (int) itable_lookup(job_ids, i);
        struct batch_job_info* jobinfo = (struct batch_job_info*) itable_lookup(job_times, k);
        jobinfo->finished = time(0);
        if (WIFEXITED(status)) {
            jobinfo->exited_normally = 1;
            jobinfo->exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            jobinfo->exited_normally = 0;
            jobinfo->exit_signal = WTERMSIG(status);
        }
		debug(D_BATCH,"%i:%s Child process %i has been found and processes, sending RANK0 the results\n",rank, procname, i);
        char* tmp = string_format("{\"ID\":%i,\"START\":%i,\"END\":%i,\"NORMAL\":%i,\"STATUS\":%i,\"SIGNAL\":%i}", k, jobinfo->started, jobinfo->finished, jobinfo->exited_normally, jobinfo->exit_code, jobinfo->exit_signal);
        len = strlen(tmp);
		unsigned* len1 = malloc(sizeof(unsigned));
		*len1 = len;
		//fprintf(stderr,"%i:%s Sending RANK 0 the length of the result string\n",rank,procname);
        MPI_Send(len1, 1, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD);
		//fprintf(stderr,"%i:%s Sending Rank 0 the result string\n",rank,procname);
        MPI_Send(tmp, len, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
		debug(D_BATCH,"%i:%s Sent the result string, freeing memory and continuing loop\n",rank,procname);
        //free(tmp);

    }

	
    MPI_Finalize();

    return 0;
}

batch_queue_stub_free(mpi);
batch_queue_stub_port(mpi);
batch_queue_stub_option_update(mpi);

batch_fs_stub_chdir(mpi);
batch_fs_stub_getcwd(mpi);
batch_fs_stub_mkdir(mpi);
batch_fs_stub_putfile(mpi);
batch_fs_stub_rename(mpi);
batch_fs_stub_stat(mpi);
batch_fs_stub_unlink(mpi);

const struct batch_queue_module batch_queue_mpi = {
    BATCH_QUEUE_TYPE_MPI,
    "mpi",

    batch_queue_mpi_create,
    batch_queue_mpi_free,
    batch_queue_mpi_port,
    batch_queue_mpi_option_update,

    {
        batch_job_mpi_submit,
        batch_job_mpi_wait,
        batch_job_mpi_remove,},

    {
        batch_fs_mpi_chdir,
        batch_fs_mpi_getcwd,
        batch_fs_mpi_mkdir,
        batch_fs_mpi_putfile,
        batch_fs_mpi_rename,
        batch_fs_mpi_stat,
        batch_fs_mpi_unlink,
    },
};

#endif

/* vim: set noexpandtab tabstop=4: */
