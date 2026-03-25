# test_cognitive_run.ps1
# Run from E:\Projects\UltraInfinity:
#   powershell -ExecutionPolicy Bypass -File test_cognitive_run.ps1

$payload = @'
{
  "intent": {
    "raw_prompt": "add a hello function to main",
    "action": "add",
    "targets": [],
    "goal_summary": "Add hello function",
    "constraints": [],
    "risk_level": "medium",
    "requires_planning": true
  },
  "session": {
    "project_root": "E:/Projects/UltraInfinity",
    "governance": {
      "require_plan_approval": true,
      "require_action_approval": false,
      "max_iterations": 5,
      "protected_paths": [],
      "forbidden_actions": []
    },
    "policy": {
      "risk_tolerance": "medium",
      "strategy_style": "balanced",
      "auto_commit": false,
      "run_tests_after_change": false,
      "custom_rules": []
    }
  }
}
'@

$tempFile = "$env:TEMP\ultra_payload.json"
$payload | Out-File -Encoding utf8 -FilePath $tempFile
Write-Host "Payload written to $tempFile"
Write-Host "Running ultra cognitive_run..."
Write-Host ""

ultra cognitive_run --json (Get-Content $tempFile -Raw)