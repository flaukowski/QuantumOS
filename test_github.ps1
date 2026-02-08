# Simple test for GitHub API
$env:GITHUB_TOKEN = "REDACTED_TOKEN"

$headers = @{
    "Authorization" = "token $env:GITHUB_TOKEN"
    "Accept" = "application/vnd.github.v3+json"
}

try {
    $response = Invoke-RestMethod -Uri "https://api.github.com/user" -Headers $headers
    Write-Host "✅ Auth successful for user: $($response.login)"
    
    # Test creating a simple issue
    $simpleBody = @{
        "title" = "Test Issue"
        "body" = "This is a test issue to verify API access."
        "labels" = @("test")
    } | ConvertTo-Json
    
    $issueResponse = Invoke-RestMethod -Uri "https://api.github.com/repos/flaukowski/QuantumOS/issues" -Method Post -Headers $headers -Body $simpleBody -ContentType "application/json"
    Write-Host "✅ Test issue created: $($issueResponse.html_url)"
    Write-Host "Issue ID: $($issueResponse.number)"
    
} catch {
    Write-Host "❌ Error: $($_.Exception.Message)"
    Write-Host "Status: $($_.Exception.Response.StatusCode)"
    Write-Host "Response: $($_.Exception.Response.StatusDescription)"
}
