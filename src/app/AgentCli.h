#pragma once

#include <QStringList>

class Agent;
class Project;

// Headless entry points used by --agent and --eval (scripting, testing and
// prompt tuning without the UI).
namespace AgentCli {

// Runs requests one after another, printing the transcript. Returns 0 if
// every request finished successfully.
int runRequests(Agent &agent, Project &project, const QStringList &requests);

// Runs every case in an eval file and prints a scorecard. Cases name media
// files relative to `mediaDir`. Returns 0 if all checks pass.
int runEval(Agent &agent, Project &project, const QString &casesFile, const QString &mediaDir, const QString &only);

}  // namespace AgentCli
