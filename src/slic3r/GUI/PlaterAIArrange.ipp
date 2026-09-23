// Included once by Plater.cpp inside Slic3r::GUI; keep AI arrangement state transitions together.

bool Plater::arrange()
{
    auto &w = get_ui_job_worker();
    if (!w.is_idle())
        return false;

    p->take_snapshot(_u8L("Arrange"));
    return replace_job(w, std::make_unique<ArrangeJob>());
}

bool Plater::arrange_for_ai_workflow()
{
    m_ai_arrange_pending.store(true);
    if (!last_arrange_job_is_finished()) {
        notify_ai_arrange_finished(false);
        return false;
    }
    if (!arrange()) {
        m_arrange_running.store(false);
        m_ai_arrange_pending.store(false);
        Sidebar &workflow = sidebar();
        if (workflow.ai_workflow_active()) {
            workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Failed,
                                             _L("自动摆放任务未能启动"));
            workflow.finish_ai_workflow(false, _L("模型已导入，但自动摆放未能启动"));
        }
        return false;
    }
    return true;
}

void Plater::notify_ai_arrange_finished(bool success)
{
    if (!m_ai_arrange_pending.exchange(false))
        return;

    Sidebar &workflow = sidebar();
    if (!workflow.ai_workflow_active())
        return;

    if (!success) {
        workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Failed,
                                         _L("自动摆放失败，请检查模型与打印板"));
        workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Failed,
                                         _L("未开始"));
        workflow.update_ai_workflow_step(Sidebar::AIGCode, Sidebar::AIWorkflowStatus::Failed,
                                         _L("未生成"));
        workflow.finish_ai_workflow(false, _L("模型已导入，但自动摆放失败"));
        return;
    }

    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Success,
                                     _L("已放置到打印板"));
    workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Waiting,
                                     _L("等待手动切片"));
    workflow.update_ai_workflow_step(Sidebar::AIGCode, Sidebar::AIWorkflowStatus::Waiting,
                                     _L("手动切片后生成"));
}
