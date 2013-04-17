/* Transaction consistency:  
 *  fork a process:
 *   Open two tables, T1 and T2
 *   begin transaction
 *   store A in T1
 *   checkpoint
 *   store B in T2
 *   commit (or abort)
 *   signal to end the process abruptly
 *  wait for the process to finish
 *   open the environment doing recovery
 *   check to see if both A and B are present (or absent)
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include "test.h"

const int envflags = DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN |DB_RECOVER;
char *namea="a.db";
char *nameb="b.db";


static void
do_x1_shutdown (BOOL do_commit)
{
    int r;
    system("rm -rf " ENVDIR);
    toku_os_mkdir(ENVDIR, S_IRWXU+S_IRWXG+S_IRWXO);
    DB_ENV *env;
    DB *dba, *dbb;
    r = db_env_create(&env, 0);                                                         CKERR(r);
    r = env->open(env, ENVDIR, envflags, S_IRWXU+S_IRWXG+S_IRWXO);                      CKERR(r);
    r = db_create(&dba, env, 0);                                                        CKERR(r);
    r = dba->open(dba, NULL, namea, NULL, DB_BTREE, DB_AUTO_COMMIT|DB_CREATE, 0666);    CKERR(r);
    r = db_create(&dbb, env, 0);                                                        CKERR(r);
    r = dbb->open(dbb, NULL, nameb, NULL, DB_BTREE, DB_AUTO_COMMIT|DB_CREATE, 0666);    CKERR(r);
    DB_TXN *txn;
    r = env->txn_begin(env, NULL, &txn, 0);                                             CKERR(r);
    {
	DBT a={.data="a", .size=2};
	DBT b={.data="b", .size=2};
	r = dba->put(dba, txn, &a, &b, 0);                                              CKERR(r);
	r = env->txn_checkpoint(env, 0, 0, 0);                                          CKERR(r);
	r = dbb->put(dbb, txn, &b, &a, 0);                                              CKERR(r);
    }
    //printf("opened\n");
    if (do_commit) {
	r = txn->commit(txn, 0);                                                        CKERR(r);
    }
    //printf("shutdown\n");
    abort();
}

static void
do_x1_recover (BOOL did_commit)
{
    DB_ENV *env;
    DB *dba, *dbb;
    int r;
    r = db_env_create(&env, 0);                                                             CKERR(r);
    r = env->open(env, ENVDIR, envflags, S_IRWXU+S_IRWXG+S_IRWXO);                          CKERR(r);
    r = db_create(&dba, env, 0);                                                            CKERR(r);
    r = dba->open(dba, NULL, namea, NULL, DB_BTREE, DB_AUTO_COMMIT|DB_CREATE, 0666);        CKERR(r);
    r = db_create(&dbb, env, 0);                                                            CKERR(r);
    r = dba->open(dbb, NULL, nameb, NULL, DB_BTREE, DB_AUTO_COMMIT|DB_CREATE, 0666);        CKERR(r);
    DBT aa={.size=0}, ab={.size=0};
    DBT ba={.size=0}, bb={.size=0};
    DB_TXN *txn;
    DBC *ca,*cb;
    r = env->txn_begin(env, NULL, &txn, 0);                                                 CKERR(r);
    r = dba->cursor(dba, txn, &ca, 0);                                                      CKERR(r);
    r = dbb->cursor(dbb, txn, &cb, 0);                                                      CKERR(r);
    int ra = ca->c_get(ca, &aa, &ab, DB_FIRST);                                             CKERR(r);
    int rb = cb->c_get(cb, &ba, &bb, DB_FIRST);                                             CKERR(r);
    if (did_commit) {
	assert(ra==0);
	assert(rb==0);
	// verify key-value pairs
	assert(aa.size==2);
	assert(ab.size==2);
	assert(ba.size==2);
	assert(bb.size==2);
	unsigned int i;
	char a[2] = "a";
	char b[2] = "b";
	for (i=0;i<2;i++) { 
	  assert(*(char*)(aa.data + i) == a[i]); 
	  assert(*(char*)(ab.data + i) == b[i]); 
	  assert(*(char*)(ba.data + i) == b[i]); 
	  assert(*(char*)(bb.data + i) == a[i]); 
	}
	// make sure no other entries in DB
	assert(ca->c_get(ca, &aa, &ab, DB_NEXT) == DB_NOTFOUND);
	assert(cb->c_get(cb, &ba, &bb, DB_NEXT) == DB_NOTFOUND);

	fprintf(stderr, "Both verified. Yay!\n");
    } else {
	// It wasn't commited (it also wasn't aborted), but a checkpoint happened.
	assert(ra==DB_NOTFOUND);
	assert(rb==DB_NOTFOUND);
	fprintf(stderr, "Neither present. Yay!\n");
    }
    r = ca->c_close(ca);                                                                    CKERR(r);
    r = cb->c_close(cb);                                                                    CKERR(r);
    r = txn->commit(txn, 0);                                                                CKERR(r);
    r = dba->close(dba, 0);                                                                 CKERR(r);
    r = dbb->close(dbb, 0);                                                                 CKERR(r);
    r = env->close(env, 0);                                                                 CKERR(r);
    exit(0);
}

const char *cmd;

static void
do_test_internal (BOOL commit)
{
    pid_t pid;
    if (0 == (pid=fork())) {
	int r=execl(cmd, verbose ? "-v" : "-q", commit ? "--commit" : "--abort", NULL);
	assert(r==-1);
	printf("execl failed: %d (%s)\n", errno, strerror(errno));
	assert(0);
    }
    {
	int r;
	int status;
	r = waitpid(pid, &status, 0);
	//printf("signaled=%d sig=%d\n", WIFSIGNALED(status), WTERMSIG(status));
	assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    }
    // Now find out what happend
    
    if (0 == (pid = fork())) {
	int r=execl(cmd, verbose ? "-v" : "-q", commit ? "--recover-commited" : "--recover-aborted", NULL);
	assert(r==-1);
	printf("execl failed: %d (%s)\n", errno, strerror(errno));
	assert(0);
    }
    {
	int r;
	int status;
	r = waitpid(pid, &status, 0);
	//printf("recovery exited=%d\n", WIFEXITED(status));
	assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
}

static void
do_test (void) {
    do_test_internal(TRUE);
    do_test_internal(FALSE);
}

BOOL do_commit=FALSE, do_abort=FALSE, do_recover_commited=FALSE,  do_recover_aborted=FALSE;

static void
x1_parse_args (int argc, char *argv[]) {
    int resultcode;
    cmd = argv[0];
    argc--; argv++;
    while (argc>0) {
	if (strcmp(argv[0], "-v") == 0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose--;
	    if (verbose<0) verbose=0;
	} else if (strcmp(argv[0],"--abort")==0) {
	    do_abort=1;
	} else if (strcmp(argv[0],"--commit")==0) {
	    do_commit=1;
	} else if (strcmp(argv[0],"--recover-commited")==0) {
	    do_recover_commited=1;
	} else if (strcmp(argv[0],"--recover-aborted")==0) {
	    do_recover_aborted=1;
	} else if (strcmp(argv[0], "-h")==0) {
	    resultcode=0;
	do_usage:
	    fprintf(stderr, "Usage:\n%s [-v|-q]* [-h] {--abort | --commit | --recover-commited | --recover-aborted } \n", cmd);
	    exit(resultcode);
	} else {
	    fprintf(stderr, "Unknown arg: %s\n", argv[0]);
	    resultcode=1;
	    goto do_usage;
	}
	argc--;
	argv++;
    }
    {
	int n_specified=0;
	if (do_commit)            n_specified++;
	if (do_abort)             n_specified++;
	if (do_recover_commited)  n_specified++;
	if (do_recover_aborted)   n_specified++;
	if (n_specified>1) {
	    printf("Specify only one of --commit or --abort or --recover-commited or --recover-aborted\n");
	    resultcode=1;
	    goto do_usage;
	}
    }
}

int
test_main (int argc, char *argv[])
{
    x1_parse_args(argc, argv);
    if (do_commit) {
	do_x1_shutdown (TRUE);
    } else if (do_abort) {
	do_x1_shutdown (FALSE);
    } else if (do_recover_commited) {
	do_x1_recover(TRUE);
    } else if (do_recover_aborted) {
	do_x1_recover(FALSE);
    } else {
	do_test();
    }
    return 0;
}
