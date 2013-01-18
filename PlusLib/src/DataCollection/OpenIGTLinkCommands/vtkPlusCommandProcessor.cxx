/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/ 

#include "PlusConfigure.h"

#include "vtkXMLUtilities.h"

#include "vtkPlusCommandProcessor.h"
#include "vtkRecursiveCriticalSection.h"

#include "vtkPlusCommand.h"
#include "vtkPlusStartStopRecordingCommand.h"
#include "vtkPlusReconstructVolumeCommand.h"

vtkStandardNewMacro( vtkPlusCommandProcessor );

//----------------------------------------------------------------------------
vtkPlusCommandProcessor::vtkPlusCommandProcessor()
: Threader(vtkSmartPointer<vtkMultiThreader>::New())
, Mutex(vtkSmartPointer<vtkRecursiveCriticalSection>::New())
, CommandExecutionThreadId(-1)
, CommandExecutionActive(std::make_pair(false,false))
, DataCollector(NULL)
{
  // Register default commands
  {
    vtkPlusCommand* cmd=vtkPlusStartStopRecordingCommand::New();
    RegisterPlusCommand(cmd);
    cmd->Delete();
  }
  {
    vtkPlusCommand* cmd=vtkPlusReconstructVolumeCommand::New();
    RegisterPlusCommand(cmd);
    cmd->Delete();
  }
}

//----------------------------------------------------------------------------
vtkPlusCommandProcessor::~vtkPlusCommandProcessor()
{
  for (std::map<std::string,vtkPlusCommand*>::iterator it=this->RegisteredCommands.begin(); it!=this->RegisteredCommands.end(); ++it)
  {
    (it->second)->UnRegister(this); 
    (it->second)=NULL; 
  } 
  this->RegisteredCommands.clear();

  SetDataCollector(NULL);
}

//----------------------------------------------------------------------------
void vtkPlusCommandProcessor::PrintSelf( ostream& os, vtkIndent indent )
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Available Commands : ";
  // TODO: print registered commands
  /*
  if( AvailableCommands )
  {
    AvailableCommands->PrintSelf( os, indent );
  }
  else
  {
    os << "None.";
  }
  */
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusCommandProcessor::Start()
{
  if ( this->CommandExecutionThreadId < 0 )
  {
    this->CommandExecutionActive.first = true;
    this->CommandExecutionThreadId = this->Threader->SpawnThread( (vtkThreadFunctionType)&CommandExecutionThread, this );
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusCommandProcessor::Stop()
{
  // Stop the command execution thread
  if ( this->CommandExecutionThreadId >=0 )
  {
    this->CommandExecutionActive.first = false; 
    while ( this->CommandExecutionActive.second )
    {
      // Wait until the thread stops 
      vtkAccurateTimer::Delay( 0.2 ); 
    }
    this->CommandExecutionThreadId = -1; 
  }

  LOG_DEBUG("Command execution thread stopped");

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void* vtkPlusCommandProcessor::CommandExecutionThread( vtkMultiThreader::ThreadInfo* data )
{
  vtkPlusCommandProcessor* self = (vtkPlusCommandProcessor*)( data->UserData );

  self->CommandExecutionActive.second = true;   

  // Execute commands until a stop is requested  
  while ( self->CommandExecutionActive.first )
  {
    int numberOfExecutedCommands=self->ExecuteCommands();
    if (numberOfExecutedCommands==0)
    {
      // no commands in the queue, wait a bit before checking again
      const double commandQueuePollIntervalSec=0.010;
#ifdef _WIN32
      Sleep(commandQueuePollIntervalSec*1000);
#else
      usleep(commandQueuePollIntervalSec * 1000000);
#endif
    }
  }

  // Close thread
  self->CommandExecutionThreadId = -1;
  self->CommandExecutionActive.second = false; 
  return NULL;
}

//----------------------------------------------------------------------------
int vtkPlusCommandProcessor::ExecuteCommands()
{
  int numberOfExecutedCommands=0;
  bool isQueueEmpty=true;
  {
    PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
    if (this->ActiveCommands.empty())
    {
      return numberOfExecutedCommands;
    }
  }

  PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
  for (std::deque< vtkPlusCommand* >::iterator cmdIt=this->ActiveCommands.begin(); cmdIt!=this->ActiveCommands.end(); ++cmdIt)
  {
    numberOfExecutedCommands++;
    (*cmdIt)->Execute();
    if ((*cmdIt)->IsCompleted())
    {
      // the command execution is completed, so remove it from the queue of active commands
      (*cmdIt)->UnRegister(this);
      cmdIt=this->ActiveCommands.erase(cmdIt);
      if (cmdIt==this->ActiveCommands.end())
      {
        // it was the last command in the queue
        break;
      }
    }
  }
  return numberOfExecutedCommands;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusCommandProcessor::RegisterPlusCommand(vtkPlusCommand* cmd)
{
  if (cmd==NULL)
  {
    LOG_ERROR("vtkPlusCommandProcessor::RegisterPlusCommand received an invalid command object");
    return PLUS_FAIL;
  }
  std::list<std::string> cmdNames;
  cmd->GetCommandNames(cmdNames);
  if (cmdNames.empty())
  {
    LOG_ERROR("Cannot register command: command name is empty");
    return PLUS_FAIL;
  }
  for (std::list<std::string>::iterator nameIt=cmdNames.begin(); nameIt!=cmdNames.end(); ++nameIt)
  {
    this->RegisteredCommands[*nameIt]=cmd;
    cmd->Register(this);
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
vtkPlusCommand* vtkPlusCommandProcessor::CreatePlusCommand(const std::string &commandStr)
{
  vtkSmartPointer<vtkXMLDataElement> cmdElement = vtkSmartPointer<vtkXMLDataElement>::Take( vtkXMLUtilities::ReadElementFromString(commandStr.c_str()) );
  if (cmdElement.GetPointer()==NULL)
  {
    LOG_ERROR("failed to parse XML command string (received: "+commandStr+")");
    return NULL;
  }
  if (STRCASECMP(cmdElement->GetName(),"Command")!=0)
  {
    LOG_ERROR("Command element expected (received: "+commandStr+")");
    return NULL;
  }
  const char* cmdName=cmdElement->GetAttribute("Name");
  if (cmdName==NULL)
  {
    LOG_ERROR("Command element's Name attribute is missing (received: "+commandStr+")");
    return NULL;
  }
  if (this->RegisteredCommands.find(cmdName) == this->RegisteredCommands.end())
  {
    // unregistered command
    LOG_ERROR("Unknown command: "<<cmdName);
    return NULL;
  }
  vtkPlusCommand* cmd=(this->RegisteredCommands[cmdName])->Clone();
  if (cmd->ReadConfiguration(cmdElement)!=PLUS_SUCCESS)
  {
    cmd->Delete();
    cmd=NULL;
    LOG_ERROR("Failed to initialize command from string: "+commandStr);
    return NULL;
  }  
  return cmd;
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusCommandProcessor::QueueCommand(unsigned int clientId, const std::string &commandString)
{
  if (commandString.empty())
  {
    LOG_ERROR("Command string is undefined");
    return PLUS_FAIL;
  }
  vtkPlusCommand* cmd=CreatePlusCommand(commandString);
  if (cmd==NULL)
  {
    LOG_ERROR("Failed to create command from string: "+commandString);
    return PLUS_FAIL;
  }
  cmd->SetCommandProcessor(this);
  cmd->SetClientId(clientId);
  {
    // Add command to the execution queue
    PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
    this->ActiveCommands.push_back(cmd);
  }
  return PLUS_SUCCESS;
}

//------------------------------------------------------------------------------
void vtkPlusCommandProcessor::QueueReply(int clientId, PlusStatus replyStatus, const std::string& replyString)
{
  PlusCommandReply reply;
  reply.ClientId=clientId;
  reply.ReplyString=replyString;
  {
    // Add reply to the sending queue
    PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
    this->CommandReplies.push_back(reply);
  }
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusCommandProcessor::GetCommandReplies(PlusCommandReplyList &replies)
{
  {
    // Add reply to the sending queue
    PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
    replies=this->CommandReplies;
    this->CommandReplies.clear();
  }
  return PLUS_SUCCESS;
}

//------------------------------------------------------------------------------
bool vtkPlusCommandProcessor::IsRunning()
{
  return this->CommandExecutionActive.second;
}