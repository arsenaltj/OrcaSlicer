    using AIWorkflowStatus = Slic3r::GUI::AIWorkflowStatus;

    enum AIWorkflowStep : size_t
    {
        AIImportModel = 0,
        AICheckMesh,
        AIProcessColors,
        AIArrange,
        AISlice,
        AIGCode,
        AIWorkflowStepCount
    };
