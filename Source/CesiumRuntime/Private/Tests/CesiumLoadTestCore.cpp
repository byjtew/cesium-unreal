// Copyright 2020-2024 CesiumGS, Inc. and Contributors

#if WITH_EDITOR

#include "CesiumLoadTestCore.h"

#include "CesiumAsync/ICacheDatabase.h"
#include "CesiumRuntime.h"

#include "CesiumTestHelpers.h"
#include "Editor.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"

namespace Cesium {

void LoadTestContext::reset() {
  testName.Reset();
  testPasses.clear();
  creationContext = playContext = SceneGenerationContext();
  reportStep = nullptr;
}

LoadTestContext gLoadTestContext;

bool TimeLoadingCommand::Update() {

  if (!pass.testInProgress) {
    CesiumTestHelpers::pushAllowTickInEditor();

    // Set up the world for this pass
    playContext.syncWorldCamera();
    if (pass.setupStep)
      pass.setupStep(playContext, pass.optionalParameter);

    // Start test mark, turn updates back on
    pass.startMark = FPlatformTime::Seconds();
    UE_LOG(LogCesium, Display, TEXT("-- Load start mark -- %s"), *loggingName);

    playContext.setSuspendUpdate(false);

    pass.testInProgress = true;

    // Return, let world tick
    return false;
  }

  double timeMark = FPlatformTime::Seconds();

  pass.elapsedTime = timeMark - pass.startMark;

  // The command is over if tilesets are loaded, or timed out
  // Wait for a maximum of 30 seconds
  const size_t testTimeout = 30;
  bool tilesetsloaded = playContext.areTilesetsDoneLoading();
  bool timedOut = pass.elapsedTime >= testTimeout;

  if (timedOut) {
    UE_LOG(
        LogCesium,
        Error,
        TEXT("TIMED OUT: Loading stopped after %.2f seconds"),
        pass.elapsedTime);
    // Command is done
    pass.testInProgress = false;

    CesiumTestHelpers::popAllowTickInEditor();

    return true;
  }

  if (tilesetsloaded) {
    // Run verify step as part of timing
    // This is useful for running additional logic after a load, or if the step
    // exists in the pass solely, timing very specific functionality (like
    // terrain queries)
    bool verifyComplete = true;
    if (pass.verifyStep)
      verifyComplete =
          pass.verifyStep(creationContext, playContext, pass.optionalParameter);

    if (verifyComplete) {
      pass.endMark = FPlatformTime::Seconds();
      UE_LOG(LogCesium, Display, TEXT("-- Load end mark -- %s"), *loggingName);

      pass.elapsedTime = pass.endMark - pass.startMark;
      UE_LOG(
          LogCesium,
          Display,
          TEXT("Pass completed in %.2f seconds"),
          pass.elapsedTime);

      pass.testInProgress = false;

      CesiumTestHelpers::popAllowTickInEditor();

      // Command is done
      return true;
    }
  }

  // Let world tick, we'll come back to this command
  return false;
}

bool LoadTestScreenshotCommand::Update() {
  UE_LOG(
      LogCesium,
      Display,
      TEXT("Requesting screenshot to /Saved/Screenshots/WindowsEditor..."));

  // Add a dash to separate name from unique index of screen shot
  // Also add a dot to keep the base path logic from stripping away too much
  FString requestFilename = screenshotName + "-" + ".";
  FScreenshotRequest::RequestScreenshot(requestFilename, false, true);
  return true;
}

void defaultReportStep(const std::vector<TestPass>& testPasses) {
  FString reportStr;
  reportStr += "\n\nTest Results\n";
  reportStr += "-----------------------------\n";
  reportStr += "(measured time) - (pass name)\n";
  std::vector<TestPass>::const_iterator it;
  for (it = testPasses.begin(); it != testPasses.end(); ++it) {
    const TestPass& pass = *it;
    reportStr +=
        FString::Printf(TEXT("%.2f secs - %s\n"), pass.elapsedTime, *pass.name);
  }
  reportStr += "-----------------------------\n";

  UE_LOG(LogCesium, Display, TEXT("%s"), *reportStr);
}

bool TestCleanupCommand::Update() {
  // Tag the fastest pass
  if (context.testPasses.size() > 0) {
    size_t fastestPass = 0;
    double fastestTime = -1.0;
    for (size_t index = 0; index < context.testPasses.size(); ++index) {
      const TestPass& pass = context.testPasses[index];
      if (fastestTime == -1.0 || pass.elapsedTime < fastestTime) {
        fastestPass = index;
        fastestTime = pass.elapsedTime;
      }
    }
    context.testPasses[fastestPass].isFastest = true;
  }

  if (context.reportStep)
    context.reportStep(context.testPasses);
  else
    defaultReportStep(context.testPasses);

  return true;
}

bool InitForPlayWhenReady::Update() {
  if (!GEditor || !GEditor->IsPlayingSessionInEditor())
    return false;
  UE_LOG(LogCesium, Display, TEXT("Play in Editor ready..."));
  playContext.initForPlay(creationContext);
  return true;
}

bool SetPlayerViewportSize::Update() {
  for (auto playerControllerIt =
           playContext.world->GetPlayerControllerIterator();
       playerControllerIt;
       playerControllerIt++) {
    const TWeakObjectPtr<APlayerController> pPlayerController =
        *playerControllerIt;
    if (pPlayerController == nullptr) {
      continue;
    }

    const APlayerCameraManager* pPlayerCameraManager =
        pPlayerController->PlayerCameraManager;

    if (!pPlayerCameraManager) {
      continue;
    }

    ULocalPlayer* LocPlayer = Cast<ULocalPlayer>(pPlayerController->Player);
    if (LocPlayer && LocPlayer->ViewportClient &&
        LocPlayer->ViewportClient->Viewport) {
      LocPlayer->ViewportClient->Viewport->SetInitialSize(
          FIntPoint(viewportWidth, viewportHeight));
    }
  }

  return true;
}

bool RunLoadTest(
    const FString& testName,
    std::function<void(SceneGenerationContext&)> locationSetup,
    const std::vector<TestPass>& testPasses,
    int viewportWidth,
    int viewportHeight,
    ReportCallback optionalReportStep) {

  LoadTestContext& context = gLoadTestContext;

  context.reset();

  context.testName = testName;
  context.testPasses = testPasses;
  context.reportStep = optionalReportStep;

  //
  // Programmatically set up the world
  //
  UE_LOG(LogCesium, Display, TEXT("Creating common world objects..."));
  createCommonWorldObjects(context.creationContext);

  // Configure location specific objects
  UE_LOG(LogCesium, Display, TEXT("Setting up location..."));
  locationSetup(context.creationContext);
  context.creationContext.trackForPlay();

  // Halt tileset updates and reset them
  context.creationContext.setSuspendUpdate(true);
  context.creationContext.refreshTilesets();

  // Let the editor viewports see the same thing the test will
  context.creationContext.syncWorldCamera();

  //
  // Start async commands
  //

  // Wait for shaders. Shader compiles could affect performance
  ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling);

  // Queue play in editor and set desired viewport size
  FRequestPlaySessionParams Params;
  Params.WorldType = EPlaySessionWorldType::PlayInEditor;
  Params.EditorPlaySettings = NewObject<ULevelEditorPlaySettings>();
  Params.EditorPlaySettings->NewWindowWidth = viewportWidth;
  Params.EditorPlaySettings->NewWindowHeight = viewportHeight;
  Params.EditorPlaySettings->EnableGameSound = false;
  Params.EditorPlaySettings->SetClientWindowSize(
      FIntPoint(viewportWidth, viewportHeight));
  GEditor->RequestPlaySession(Params);

  // Wait until PIE is ready
  ADD_LATENT_AUTOMATION_COMMAND(
      InitForPlayWhenReady(context.creationContext, context.playContext));

  // Make sure the player viewport is the correct size. This will not be the
  // case otherwise in headless UE 5.5.
  ADD_LATENT_AUTOMATION_COMMAND(SetPlayerViewportSize(
      context.creationContext,
      context.playContext,
      viewportWidth,
      viewportHeight));

  // Wait to show distinct gap in profiler
  ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));

  std::vector<TestPass>::iterator it;
  for (it = context.testPasses.begin(); it != context.testPasses.end(); ++it) {
    TestPass& pass = *it;

    // Do our timing capture
    FString loggingName = testName + "-" + pass.name;

    ADD_LATENT_AUTOMATION_COMMAND(TimeLoadingCommand(
        loggingName,
        context.creationContext,
        context.playContext,
        pass));

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));

    FString screenshotName = testName + "-" + pass.name;
    ADD_LATENT_AUTOMATION_COMMAND(LoadTestScreenshotCommand(screenshotName))

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
  }

  // End play in editor
  ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());

  ADD_LATENT_AUTOMATION_COMMAND(TestCleanupCommand(context));

  return true;
}

}; // namespace Cesium

#endif
