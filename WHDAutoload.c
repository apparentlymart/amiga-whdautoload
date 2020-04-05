#include <clib/alib_protos.h>
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/icon_protos.h>
#include <clib/utility_protos.h>
#include <dos/dostags.h>
#include <stdio.h>
#include <string.h>
#include <workbench/startup.h>

struct Path {
  BPTR path_Next;
  BPTR path_Lock;
};

struct Library* IconBase = NULL;
struct Library* UtilityBase = NULL;

char* whdload_name = "WHDLoad";
BPTR whdload_seg_list = 0;
struct FileLock* whdload_dir_lock_to_free = 0;  // only populated if we own it

struct MsgPort* reply_port = NULL;

void close_libs() {
  if (IconBase != NULL) {
    CloseLibrary(IconBase);
    IconBase = NULL;
  }
  if (UtilityBase != NULL) {
    CloseLibrary(UtilityBase);
    UtilityBase = NULL;
  }
}

int clean_up(int status) {
  if (whdload_seg_list != 0) {
    UnLoadSeg(whdload_seg_list);
  }
  if (whdload_dir_lock_to_free != NULL) {
    UnLock(MKBADDR(whdload_dir_lock_to_free));
  }
  if (reply_port != NULL) {
    DeleteMsgPort(reply_port);
  }
  close_libs();
  return status;
}

int exit_error(char* message) {
  Printf("%s\n", message);
  return clean_up(1);
}

int exit_dos_error(char* message) {
  LONG error = IoErr();
  PrintFault(error, message);
  return clean_up(1);
}

int main(int argc, char** argv) {
  IconBase = OpenLibrary("icon.library", 37);
  if (IconBase == NULL) {
    return exit_error("Failed to load icon.library v37");
  }
  UtilityBase = OpenLibrary("utility.library", 37);
  if (UtilityBase == NULL) {
    return exit_error("Failed to load utility.library v37");
  }

  struct Process* process = (struct Process*)FindTask(NULL);
  if (process == NULL) {
    // This should never happen.
    return exit_error("Failed to find my own Process structure");
  }

  struct FileLock* wd = BADDR(process->pr_CurrentDir);
  if (wd == NULL) {
    // This should never happen.
    return exit_error("No current directory?!");
  }

  // First we'll search the current working directory for a file that
  // has a project icon whose "Default Tool" seems to be WHDLoad. The
  // tooltypes from that will be our WHDLoad local configuration.
  struct FileInfoBlock info;
  memset(&info, 0, sizeof(info));
  if (!Examine(MKBADDR(wd), &info)) {
    return exit_dos_error("Failed to examine current working directory");
  }
  char* project_filename = NULL;
  LONG stack_size = 0;
  for (;;) {
    LONG success = ExNext(MKBADDR(wd), &info);
    if (!success) {
      if (IoErr() == ERROR_NO_MORE_ENTRIES) {
        break;  // All done!
      }
      return exit_dos_error("Failed to examine item in working directory");
    }
    if (info.fib_DirEntryType >= 0) {
      // Not a file, so can't be what we are looking for.
      continue;
    }

    // We know we're looking for an icon, so the filename must end with .info
    char* filename = info.fib_FileName;
    size_t len = strlen(filename);
    if (len < 5) {
      continue;  // Not long enough for an ".info" suffix
    }
    char* suffix = filename + (len - 5);
    if (strcmp(suffix, ".info") != 0) {
      // Doesn't have a .info suffix.
      continue;
    }

    // icon.library wants the path _without_ the .info suffix, so we'll write
    // a null over the period to effectively truncate the string.
    suffix[0] = 0;

    struct DiskObject* icon = GetDiskObject(filename);
    if (icon == NULL) {
      // Invalid icon, I guess?
      continue;
    }

    // Must be a project whose default tool looks like it's trying to run
    // WHDLoad. ("Trying to run" is a heuristic, because there are lots of
    // ways to express that.)
    if (icon->do_Type == WBPROJECT) {
      char* dt = icon->do_DefaultTool;
      if (dt != NULL) {
        // Convert the default tool string to lowercase for comparison.
        for (char* c = dt; *c != 0; c++) {
          *c = ToLower(*c);
        }

        // Our heuristic here is that the default tool filename is "whdload",
        // regardless of what path it's in. That should be good enough to
        // find the one interesting file in a typical WHDLoad install dir.
        char* suffix = PathPart(dt);
        if (strcmp(suffix, "whdload") == 0) {
          // If we get here then this icon is the one we're going to use.
          project_filename = filename;
          stack_size = icon->do_StackSize;
          break;
        }
      }
    }

    FreeDiskObject(icon);
  }

  if (project_filename == NULL) {
    return exit_error("Can't find an icon that runs WHDLoad here");
  }

  // Now we've selected our project file, we need to find a WHDLoad executable
  // to run. We'll do that by searching the path associated with this program
  // (but we can do that only if we're a CLI program.)
  struct CommandLineInterface* cli = Cli();
  if (cli == NULL) {
    return exit_error("WHDAutoLoad must be run from a CLI");
  }

  char whdload_path[60];
  BPTR seg_list_bcpl = 0;
  struct FileLock* whdload_dir_lock = NULL;
  for (struct Path* entry = BADDR(cli->cli_CommandDir); entry != NULL;
       entry = BADDR(entry->path_Next)) {
    // size minus seven here because we're going to append "WHDLoad" on
    // the end of this.
    BOOL success =
        NameFromLock(entry->path_Lock, whdload_path, sizeof(whdload_path) - 7);
    if (!success) {
      continue;
    }
    size_t len = strlen(whdload_path);
    strcpy(whdload_path + len, whdload_name);

    whdload_seg_list = LoadSeg(whdload_path);
    if (whdload_seg_list != 0) {
      whdload_dir_lock = BADDR(entry->path_Lock);
      break;
    }
  }
  if (whdload_seg_list == 0) {
    // If the path didn't help, we'll try "C:"
    whdload_seg_list = LoadSeg("C:WHDLoad");
    if (whdload_seg_list != 0) {
      whdload_dir_lock = BADDR(Lock("C:", ACCESS_READ));
      if (whdload_dir_lock == NULL) {
        return exit_dos_error("Failed to lock C: directory");
      }
      whdload_dir_lock_to_free = whdload_dir_lock;
    }
  }
  if (whdload_seg_list == 0) {
    return exit_error("Can't find WHDLoad");
  }

  reply_port = CreateMsgPort();
  if (reply_port == NULL) {
    return exit_error("Failed to create message port");
  }

  BPTR home_dir = DupLock(MKBADDR(wd));
  if (home_dir == 0) {
    return exit_error("Failed to duplicate home directory lock");
  }

  const struct TagItem tags[] = {{NP_Seglist, (ULONG)whdload_seg_list},
                                 {NP_Name, (ULONG)whdload_name},
                                 {NP_HomeDir, (ULONG)home_dir},
                                 {NP_StackSize, (ULONG)stack_size},
                                 {NP_Priority, 0},
                                 {NP_Input, 0},
                                 {NP_Output, 0},
                                 {NP_CurrentDir, 0},
                                 {TAG_DONE, 0}};
  struct Process* whdload_proc = CreateNewProc(tags);
  if (whdload_proc == NULL) {
    return exit_dos_error("Failed to launch WHDLoad");
  }
  whdload_seg_list = 0;  // belongs to the other process now

  // We'll send WHDLoad a WBStartup message as if we'd launched our project
  // icon from Workbench.
  struct WBStartup* startup =
      AllocVec(sizeof(struct WBStartup), MEMF_PUBLIC | MEMF_CLEAR);
  startup->sm_Message.mn_Length = sizeof(struct WBStartup);
  startup->sm_Message.mn_ReplyPort = reply_port;
  startup->sm_Process = &whdload_proc->pr_MsgPort;
  startup->sm_Segment = whdload_seg_list;
  startup->sm_NumArgs = 2;
  startup->sm_ToolWindow = NULL;
  struct WBArg args[2];
  startup->sm_ArgList = &args[0];
  args[0].wa_Lock = MKBADDR(whdload_dir_lock);
  args[0].wa_Name = whdload_name;
  args[1].wa_Lock = MKBADDR(wd);
  args[1].wa_Name = project_filename;

  PutMsg(&whdload_proc->pr_MsgPort, &startup->sm_Message);
  for (;;) {
    WaitPort(reply_port);
    struct Message* msg = GetMsg(reply_port);
    if (msg == &startup->sm_Message) {
      break;  // We got the reply, so we're done.
    }
  }
  FreeVec(startup);

  return clean_up(0);
}
