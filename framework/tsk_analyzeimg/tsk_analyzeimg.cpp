/*
*
*  The Sleuth Kit
*
*  Contact: Brian Carrier [carrier <at> sleuthkit [dot] org]
*  Copyright (c) 2011-2012 Basis Technology Corporation. All Rights
*  reserved.
*
*  This software is distributed under the Common Public License 1.0
*/
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>

#include "tsk3/tsk_tools_i.h" // Needed for tsk_getopt
#include "framework.h"
#include "Services/TskSchedulerQueue.h"
#include "Services/TskSystemPropertiesImpl.h"
#include "Services/TskImgDBSqlite.h"
#include "File/TskFileManagerImpl.h"
#include "Extraction/TskCarvePrepSectorConcat.h"

#ifdef TSK_WIN32
#include <Windows.h>
#else
#error "Only Windows is currently supported"
#endif

#include "Poco/File.h"

static uint8_t 
makeDir(const TSK_TCHAR *dir) 
{
#ifdef TSK_WIN32
    if (CreateDirectoryW(dir, NULL) == 0) {
        fprintf(stderr, "Error creating directory: %d\n", GetLastError());
        return 1;
    }
#else

#endif
    return 0;
}

void 
usage(const char *program) 
{
    fprintf(stderr, "%s [-c framework_config_file] [-p pipeline_config_file] [-d outdir] [-vV] image_name\n", program);
    fprintf(stderr, "\t-c framework_config_file: Path to XML framework config file\n");
    fprintf(stderr, "\t-p pipeline_config_file: Path to XML pipeline config file (overrides pipeline config specified with -c)\n");
    fprintf(stderr, "\t-d outdir: Path to output directory\n");
    fprintf(stderr, "\t-v: Enable verbose mode to get more debug information\n");
    fprintf(stderr, "\t-V: Display the tool version\n");
    exit(1);
}

// get the current directory
// @@@ TODO: This should move into a framework utility
static std::wstring getProgDir()
{
    wchar_t progPath[256];
    wchar_t fullPath[256];
    
    GetModuleFileNameW(NULL, fullPath, 256);
    for (int i = wcslen(fullPath)-1; i > 0; i--) {
        if (i > 256)
            break;

        if (fullPath[i] == '\\') {
            wcsncpy_s(progPath, fullPath, i+1);
            progPath[i+1] = '\0';
            break;
        }
    }
    return std::wstring(progPath);
}

int main(int argc, char **argv1)
{
    TSK_TCHAR **argv;
    extern int OPTIND;
    int ch;
    struct STAT_STR stat_buf;
    TSK_TCHAR *pipeline_config = NULL;
    TSK_TCHAR *framework_config = NULL;
    std::wstring outDirPath;

#ifdef TSK_WIN32
    // On Windows, get the wide arguments (mingw doesn't support wmain)
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL) {
        fprintf(stderr, "Error getting wide arguments\n");
        exit(1);
    }
#else
    argv = (TSK_TCHAR **) argv1;
#endif

    while ((ch =
        GETOPT(argc, argv, _TSK_T("d:c:p:vV"))) > 0) {
        switch (ch) {
        case _TSK_T('?'):
        default:
            TFPRINTF(stderr, _TSK_T("Invalid argument: %s\n"),
                argv[OPTIND]);
            usage(argv1[0]);
        case _TSK_T('c'):
            framework_config = OPTARG;
            break;
        case _TSK_T('p'):
            pipeline_config = OPTARG;
            break;
        case _TSK_T('v'):
            tsk_verbose++;
            break;
        case _TSK_T('V'):
            tsk_version_print(stdout);
            exit(0);
            break;
        case _TSK_T('d'):
            outDirPath.assign(OPTARG);
            break;
        }
    }

    /* We need at least one more argument */
    if (OPTIND == argc) {
        tsk_fprintf(stderr, "Missing image name\n");
        usage(argv1[0]);
    }

    TSK_TCHAR *imagePath = argv[OPTIND];

    // Load the framework config if they specified it
    if (framework_config) {
        // Initialize properties based on the config file.
        TskSystemPropertiesImpl *systemProperties = new TskSystemPropertiesImpl();    
        systemProperties->initialize(framework_config);
        TskServices::Instance().setSystemProperties(*systemProperties);
    }
    else {
        Poco::File config("framework_config.xml");
        if (config.exists()) {
            TskSystemPropertiesImpl *systemProperties = new TskSystemPropertiesImpl();    
            systemProperties->initialize("framework_config.xml");
            TskServices::Instance().setSystemProperties(*systemProperties);
        }
        else {
            LOGINFO(L"No framework config file found");
        }
    }

    TSK_SYS_PROP_SET(TskSystemProperties::PROG_DIR, getProgDir()); 

    if (outDirPath == _TSK_T("")) {
        outDirPath.assign(imagePath);
        outDirPath.append(_TSK_T("_tsk_out"));
    }
    if (TSTAT(outDirPath.c_str(), &stat_buf) == 0) {
        fprintf(stderr, "Output directory already exists (%"PRIttocTSK")\n", outDirPath.c_str());
        return 1;
    }

    if (makeDir(outDirPath.c_str())) {
        return 1;
    }

    // @@@ Not UNIX-friendly
    TSK_SYS_PROP_SET(TskSystemProperties::OUT_DIR, outDirPath);

    // Create and register our SQLite ImgDB class   
    std::auto_ptr<TskImgDB> pImgDB(NULL);
    pImgDB = std::auto_ptr<TskImgDB>(new TskImgDBSqlite(outDirPath.c_str()));
    if (pImgDB->initialize() != 0) {
        fprintf(stderr, "Error initializing SQLite database\n");
        tsk_error_print(stderr);
        return 1;
    }

    // @@@ Call pImgDB->addToolInfo() as needed to set version info...

    TskServices::Instance().setImgDB(*pImgDB);

    // Create a Blackboard and register it with the framework.
    TskServices::Instance().setBlackboard((TskBlackboard &) TskDBBlackboard::instance());

    // @@@ Not UNIX-friendly
    if (pipeline_config != NULL) 
        TSK_SYS_PROP_SET(TskSystemProperties::PIPELINE_CONFIG, pipeline_config);

    // Create a Scheduler and register it
    TskSchedulerQueue scheduler;
    TskServices::Instance().setScheduler(scheduler);

    // Create an ImageFile and register it with the framework.
    TskImageFileTsk imageFileTsk;
    if (imageFileTsk.open(imagePath) != 0) {
        fprintf(stderr, "Error opening image: %"PRIttocTSK"\n", imagePath);
        tsk_error_print(stderr);
        return 1;
    }
    TskServices::Instance().setImageFile(imageFileTsk);

    // Create a FileManager and register it with the framework.
    TskServices::Instance().setFileManager(TskFileManagerImpl::instance());

    // Let's get the pipelines setup to make sure there are no errors.
    TskPipelineManager pipelineMgr;
    TskPipeline *filePipeline;
    try {
        filePipeline = pipelineMgr.createPipeline(TskPipelineManager::FILE_ANALYSIS_PIPELINE);
    }
    catch (TskException &e ) {
        fprintf(stderr, "Error creating file analysis pipeline\n");
        std::cerr << e.message() << endl;
        filePipeline = NULL;
    }

    TskPipeline *reportPipeline;
    try {
        reportPipeline = pipelineMgr.createPipeline(TskPipelineManager::REPORTING_PIPELINE);
    }
    catch (TskException &e ) {
        fprintf(stderr, "Error creating reporting pipeline\n");
        std::cerr << e.message() << endl;
        reportPipeline = NULL;
    }

    if ((filePipeline == NULL) && (reportPipeline == NULL)) {
        fprintf(stderr, "No pipelines configured.  Stopping\n");
        exit(1);
    }

    // now we analyze the data.
    // Extract
    if (imageFileTsk.extractFiles() != 0) {
        fprintf(stderr, "Error adding file system info to database\n");
        tsk_error_print(stderr);
        return 1;
    }

    // Prepare the unallocated sectors in the image for carving.
    TskCarvePrepSectorConcat carvePrep(L"unalloc.bin", 1000000000);
    carvePrep.processSectors(false);

    //Run pipeline on all files
    if (filePipeline && !filePipeline->isEmpty()) {
        TskSchedulerQueue::task_struct *task;
        while ((task = scheduler.nextTask()) != NULL) {
            if (task->task != Scheduler::FileAnalysis)  {
                fprintf(stderr, "WARNING: Skipping task %d\n", task->task);
                continue;
            }
            //printf("processing file: %d\n", (int)task->id);
            try {
                filePipeline->run(task->id);
            }
            catch (...) {
                // error message has been logged already.
            }
        }
    }

    if (reportPipeline) {
        try {
            reportPipeline->run();
        }
        catch (...) {
            fprintf(stderr, "Error running reporting pipeline\n");
            return 1;
        }
    }

    fprintf(stderr, "image analysis complete\n");
    return 0;
}

