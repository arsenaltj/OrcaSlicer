    bool latest_main_snapshot_identity(const std::string&, UndoRedo::ActionSnapshotIdentity&) const;
    bool main_snapshot_identity_is_current(const UndoRedo::ActionSnapshotIdentity&);
    bool undo_main_snapshot_exact(const UndoRedo::ActionSnapshotIdentity&, std::string&);
    bool rollback_main_snapshot_exact(const UndoRedo::ActionSnapshotIdentity&, std::string&);
