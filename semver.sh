        # Color settings
        # Check if colors should be disabled (NO_COLOR env var is common standard)
        # Also check if output is to a terminal that supports color
        if [ -n "$NO_COLOR" ] || [ -n "$CI" ] || ! [ -t 1 ]; then
            # No color mode
            USE_COLOR=false
        else
            USE_COLOR=true
        fi

        # Define colors or empty strings based on color support
        if [ "$USE_COLOR" = true ]; then
            GREEN='\033[0;32m'
            YELLOW='\033[1;33m'
            BLUE='\033[0;34m'
            RED='\033[0;31m'
            CYAN='\033[0;36m'
            MAGENTA='\033[0;35m'
            BOLD='\033[1m'
            NC='\033[0m' # No Color
        else
            GREEN=''
            YELLOW=''
            BLUE=''
            RED=''
            CYAN=''
            MAGENTA=''
            BOLD=''
            NC=''
        fi

        # Function to safely print with or without escape sequences
        # This helps with echo -e compatibility across different systems
        safe_echo() {
            if [ "$USE_COLOR" = true ]; then
                echo -e "$@"
            else
                # When colors are disabled, use regular echo to avoid issues with -e in some environments
                echo "$@"
            fi
        }

        # Function to print section headers
        print_header() {
            safe_echo "${BLUE}=== $1 ===${NC}"
        }

        # Function to print info messages
        print_info() {
            safe_echo "${GREEN}INFO:${NC} $1"
        }

        # Function to print warning messages
        print_warning() {
            safe_echo "${YELLOW}WARNING:${NC} $1"
        }

        # Function to print debug messages (only if DEBUG is true)
        print_debug() {
            if [ "$DEBUG" = true ]; then
                safe_echo "${CYAN}DEBUG:${NC} $1"
            fi
        }

        # Function to print error messages
        print_error() {
            safe_echo "${RED}ERROR:${NC} $1" >&2
        }

        # Function to highlight version increment messages
        print_increment() {
            safe_echo "${MAGENTA}${BOLD}INCREMENT:${NC} $1"
        }

        # Function to display a list of files with indentation
        print_file_list() {
            if [ "$DEBUG" = true ]; then
                if [ -z "$1" ]; then
                    print_debug "No files changed"
                    return
                fi
                
                print_debug "Changed files:"
                echo "$1" | while IFS= read -r file; do
                    # Check if the file matches ignore patterns
                    if [ -n "$IGNORE_PATTERNS" ] && [[ "$file" =~ ($IGNORE_PATTERNS) ]]; then
                        safe_echo "  ${CYAN}  ➜ $file${NC} ${YELLOW}(ignored)${NC}"
                    else
                        safe_echo "  ${CYAN}  ➜ $file${NC}"
                    fi
                done
            fi
        }

        # Function to print result information
        print_result() {
            safe_echo "${GREEN}=== RESULT ===${NC}"
            safe_echo "${GREEN}Version:${NC}      $1"
            safe_echo "${GREEN}Full Version:${NC} $2"
            safe_echo "${GREEN}RPM Version:${NC}  $3"
            safe_echo "${GREEN}DEB Version:${NC}  $4"
            safe_echo "${GREEN}Target:${NC}       $5"
        }

        # Function to create git tag for current version
        create_version_tag() {
            local tag="$1"
            local commit="$2"
            
            # Create tag
            print_info "Creating tag $tag for commit $commit"
            git tag "$tag" "$commit"
            
            # Add tag to the list of created tags
            CREATED_TAGS+=("$tag")
            print_debug "Added tag $tag to CREATED_TAGS array (current size: ${#CREATED_TAGS[@]})"
        }

        # Function to push all created tags at once
        push_all_tags() {
            if [ ${#CREATED_TAGS[@]} -eq 0 ]; then
                print_info "No new tags to push"
                return
            fi

            # Only push if we have GitHub credentials
            if [ -n "$GITHUB_ACTOR" -a -n "$GITHUB_REPOSITORY" -a -n "$GITHUB_TOKEN" ]; then
                print_info "Pushing ${#CREATED_TAGS[@]} new tags to remote"
                
                # Join tags with commas for display
                local tags_str=$(IFS=, ; echo "${CREATED_TAGS[*]}")
                print_info "Tags to push: $tags_str"
                
                # Try to push all tags
                git push origin ${CREATED_TAGS[@]} || {
                    print_warning "Failed to push some tags, checking if the latest tag was pushed"
                }

                # Get the latest tag (last in the array)
                local latest_tag="${CREATED_TAGS[-1]}"
                
                # Check if the latest tag exists remotely
                if ! git ls-remote --tags origin "$latest_tag" | grep -q "$latest_tag"; then
                    print_error "The latest tag ($latest_tag) is not present in the remote repository. Please check if the provided token has sufficient permissions."
                    exit 1
                else
                    print_info "The latest tag ($latest_tag) is present in the remote repository"
                fi
            else
                print_warning "Not a GitHub Actions environment, skipping tag push"
            fi
        }

        # Set GITHUB_REPOSITORY if not set
        if [ -z "$GITHUB_REPOSITORY" ]; then
            # Try to determine repository from git remote
            GITHUB_REPOSITORY=$(git remote -v | grep -m 1 -o 'github.com[:/]\([^/]\+/[^/]\+\)' | sed 's/github.com[:\/]//')
            # If still not set, use a default
            if [ -z "$GITHUB_REPOSITORY" ]; then
                GITHUB_REPOSITORY="manticoresoftware/manticoresearch"
            fi
        fi
        print_info "Using repository: $GITHUB_REPOSITORY"

        # Enable case-insensitive matching
        shopt -s nocasematch

        # Function to fetch details from GitHub issue or pull request
        fetch_github_details() {
            local url_or_issue="$1"
            local repo="${GITHUB_REPOSITORY}"  # Automatically use the current repository
            local token="$GITHUB_TOKEN"  # Ensure this environment variable is set

            # Determine if the input is a full URL or an issue number
            if [[ "$url_or_issue" =~ ^https://github\.com/ ]]; then
                # Extract the repo from URL if provided
                repo_from_url=$(echo "$url_or_issue" | sed -E 's|https://github.com/([^/]+/[^/]+).*|\1|')
                # Extract the issue/PR number from the URL
                issue_number=$(echo "$url_or_issue" | grep -oE '[0-9]+' | head -n 1)
                # Always use the issues endpoint
                api_url="https://api.github.com/repos/${repo_from_url}/issues/${issue_number}"
            else
                # If just an issue number is provided (e.g., #123), use the current repository
                api_url="https://api.github.com/repos/$repo/issues/${url_or_issue/#\#/}"
            fi

            print_debug "Fetching details from: $api_url" >&2

            # Fetch details using GitHub API
            response=$(curl -s -H "Authorization: token $token" "$api_url")

            # Check for errors
            if [[ $(echo "$response" | jq -r '.message // empty') == "Not Found" ]]; then
                echo "{\"type\": \"unknown\", \"label\": \"\"}"
                return
            fi

            # Extract type and label from the response
            issue_type="unknown"
            if [[ $(echo "$response" | jq -r '.pull_request // empty') != "" ]]; then
                issue_type="pull_request"
            elif [[ $(echo "$response" | jq -r '.url // empty') == *"/issues/"* ]]; then
                issue_type="issue"
            fi

            # Look for specific labels or types
            labels=$(echo "$response" | jq -r '.labels[]?.name' 2>/dev/null || echo "")
            label=""
            
            # Check for bug/feature in labels
            if [[ "$labels" == *"bug"* ]]; then
                print_debug "Found 'bug' in labels" >&2
                label="bug"
            elif [[ "$labels" == *"feature"* ]]; then
                print_debug "Found 'feature' in labels" >&2
                label="feature"
            fi

            # If no label found, check if type field exists and extract its name
            if [[ -z "$label" ]]; then
                type_field=$(echo "$response" | jq -r '.type.name // empty' 2>/dev/null)
                print_debug "Type field: '$type_field'" >&2

                if [[ -n "$type_field" ]]; then
                    # Case-insensitive match for Bug/Feature
                    if [[ "$type_field" =~ [Bb][Uu][Gg] ]]; then
                        print_debug "Found 'Bug' in type field" >&2
                        label="bug"
                    elif [[ "$type_field" =~ [Ff][Ee][Aa][Tt][Uu][Rr][Ee] ]]; then
                        print_debug "Found 'Feature' in type field" >&2
                        label="feature"
                    fi
                fi
            fi

            # Return type and label as a JSON object
            result="{\"type\": \"$issue_type\", \"label\": \"$label\"}"
            print_debug "Returning result: $result" >&2
            echo "$result"
        }

        # Function to check if commit follows conventional commits format
        is_conventional_commit() {
            local message="$1"
            local pattern="^(feat|fix|docs|style|refactor|perf|test|build|ci|chore)(\([a-z0-9-]+\))?!?:[[:space:]]"
            # Check for conventional commit format: type(scope): description
            # or type(scope)!: description for breaking changes
            if [[ "$message" =~ $pattern ]]; then
                return 0
            fi
            return 1
        }

        # Function to check if author should use conventional commits
        should_use_conventional_commits() {
            local author="$1"
            if [ -z "$CONVENTIONAL_COMMITS_AUTHORS" ]; then
                return 1
            fi
            # Convert comma-separated list to array
            IFS=',' read -ra AUTHORS <<< "$CONVENTIONAL_COMMITS_AUTHORS"
            for a in "${AUTHORS[@]}"; do
                if [[ "$author" == *"$a"* ]]; then
                    return 0
                fi
            done
            return 1
        }

        # Function to check if commit is a merge commit
        is_merge_commit() {
            local commit=$1
            git log -1 --format="%P" "$commit" | grep -q " "
        }

        # Function to get commit info
        get_commit_info() {
            local commit=$1
            local info=$(git log -1 --format="%s|%an|%ae|%ad" --date=format:"%Y-%m-%d %H:%M:%S" "$commit")
            echo "$info"
        }

        # Function to analyze commit message for version bump
        analyze_commit_message() {
            local commit_message="$1"
            local commit_author="$2"
            local version_incremented=0
            local old_version="${MAJOR}.${MINOR}.${PATCH}"

            # 1. Validate conventional commits format if author is in the list
            if should_use_conventional_commits "$commit_author"; then
                print_debug "Author $commit_author is in conventional commits list, applying strict rules"
                if ! is_conventional_commit "$commit_message"; then
                    print_warning "Commit does not follow conventional commits format, skipping version bump"
                    return 1
                fi
            fi

            # 2. Check for breaking changes (highest priority)
            # Pattern for type!: or type(scope)!: format
            local breaking_pattern="^[a-zA-Z]+(\([^)]+\))?!:[[:space:]]"
            if [[ "$commit_message" =~ $breaking_pattern ]] || [[ "$commit_message" =~ BREAKING[[:space:]]+CHANGE: ]]; then
                print_info "Found breaking change in commit message"
                MAJOR=$((MAJOR + 1))
                MINOR=0
                PATCH=0
                print_increment "Found breaking change! Incrementing MAJOR version to $MAJOR.$MINOR.$PATCH"
                version_incremented=1
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            fi

            # 3. Check for explicit version change
            if [[ "$commit_message" =~ [Vv]ersion[[:space:]]+from[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]+to[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+) ]]; then
                old_ver="${BASH_REMATCH[1]}"
                new_ver="${BASH_REMATCH[2]}"
                print_info "Found explicit version change from $old_ver to $new_ver"
                
                # Parse old and new versions
                IFS='.' read -r old_major old_minor old_patch <<< "$old_ver"
                IFS='.' read -r new_major new_minor new_patch <<< "$new_ver"
                
                # Determine what component changed and apply the same type of increment to current version
                if [ "$new_major" -gt "$old_major" ]; then
                    print_info "MAJOR version increment detected in commit message"
                    MAJOR=$((MAJOR + 1))
                    MINOR=0
                    PATCH=0
                    version_incremented=1
                    print_increment "Applying MAJOR version increment to current version: $MAJOR.$MINOR.$PATCH"
                elif [ "$new_minor" -gt "$old_minor" ]; then
                    print_info "MINOR version increment detected in commit message"
                    MINOR=$((MINOR + 1))
                    PATCH=0
                    version_incremented=1
                    print_increment "Applying MINOR version increment to current version: $MAJOR.$MINOR.$PATCH"
                elif [ "$new_patch" -gt "$old_patch" ]; then
                    print_info "PATCH version increment detected in commit message"
                    PATCH=$((PATCH + 1))
                    version_incremented=1
                    print_increment "Applying PATCH version increment to current version: $MAJOR.$MINOR.$PATCH"
                else
                    print_warning "Version in commit message doesn't indicate an increment"
                fi

                if [ $version_incremented -eq 1 ]; then
                    CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                    return 0
                fi
            fi

            # 4. Check commit message patterns (conventional and legacy)
            # First check conventional format (excluding breaking changes which are handled above)
            local feat_pattern="^feat(\([[:alnum:]-]+\))?:[[:space:]]"
            local feat_breaking_pattern="^feat(\([[:alnum:]-]+\))?!:[[:space:]]"
            local fix_pattern="^fix(\([[:alnum:]-]+\))?:[[:space:]]"
            local fix_breaking_pattern="^fix(\([[:alnum:]-]+\))?!:[[:space:]]"
            
            if [[ "$commit_message" =~ $feat_pattern ]] && [[ ! "$commit_message" =~ $feat_breaking_pattern ]]; then
                print_info "Found conventional feature commit"
                MINOR=$((MINOR + 1))
                PATCH=0
                version_incremented=1
                print_increment "Found feature commit (conventional). Incrementing MINOR version to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            elif [[ "$commit_message" =~ $fix_pattern ]] && [[ ! "$commit_message" =~ $fix_breaking_pattern ]]; then
                print_info "Found conventional fix commit"
                PATCH=$((PATCH + 1))
                version_incremented=1
                print_increment "Found bug fix (conventional). Incrementing PATCH version to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            fi

            # Then check legacy format
            if [[ "$commit_message" =~ feature: || "$commit_message" =~ ^feature ]]; then
                print_info "Found legacy feature commit"
                MINOR=$((MINOR + 1))
                PATCH=0
                version_incremented=1
                print_increment "Found feature commit (legacy). Incrementing MINOR version to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            elif [[ "$commit_message" =~ (^|[[:space:],;:])fix ]]; then
                print_info "Found legacy fix commit"
                PATCH=$((PATCH + 1))
                version_incremented=1
                print_increment "Found bug fix (legacy). Incrementing PATCH version to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            fi

            # 5. Check GitHub references (lowest priority)
            url_or_issue=$(echo "$commit_message" | grep -oE 'https://github\.com/[a-zA-Z0-9._-]+/[a-zA-Z0-9._-]+(/[a-zA-Z0-9._/-]+)?' | head -n 1)
            if [[ -n "$url_or_issue" ]]; then
                print_info "Found issue reference: $url_or_issue"
                
                # Fetch issue or PR details using GitHub API
                details=$(fetch_github_details "$url_or_issue")
                type=$(echo "$details" | jq -r '.type')
                label=$(echo "$details" | jq -r '.label')
                
                print_info "Issue details - type: $type, label: $label"

                # Prioritize PR over issue
                if [[ "$type" == "pull_request" ]]; then
                    if [[ "$label" == "feature" ]]; then
                        print_info "Found feature PR"
                        MINOR=$((MINOR + 1))
                        PATCH=0
                        version_incremented=1
                        print_increment "Found feature PR. Incrementing MINOR version to $MAJOR.$MINOR.$PATCH"
                        CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                        return 0
                    elif [[ "$label" == "bug" ]]; then
                        print_info "Found bug PR"
                        PATCH=$((PATCH + 1))
                        version_incremented=1
                        print_increment "Found bug PR. Incrementing PATCH version to $MAJOR.$MINOR.$PATCH"
                        CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                        return 0
                    else
                        print_warning "PR has no relevant label (feature/bug)"
                    fi
                elif [[ "$type" == "issue" ]]; then
                    if [[ "$label" == "feature" ]]; then
                        print_info "Found feature issue"
                        MINOR=$((MINOR + 1))
                        PATCH=0
                        version_incremented=1
                        print_increment "Found feature issue. Incrementing MINOR version to $MAJOR.$MINOR.$PATCH"
                        CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                        return 0
                    elif [[ "$label" == "bug" ]]; then
                        print_info "Found bug issue"
                        PATCH=$((PATCH + 1))
                        version_incremented=1
                        print_increment "Found bug issue. Incrementing PATCH version to $MAJOR.$MINOR.$PATCH"
                        CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                        return 0
                    else
                        print_warning "Issue has no relevant label (feature/bug)"
                    fi
                else
                    print_warning "Unknown reference type: $type"
                fi
            fi

            # No version bump if no rules matched
            print_warning "No version bump rules matched for this commit"
            return 1
        }

        # Function to analyze multiple commits for version bump (used for merge commits)
        analyze_multiple_commits() {
            local commits="$1"
            local highest_increment="none"
            local version_incremented=0
            
            # Track the highest increment type found
            local found_major=0
            local found_minor=0
            local found_patch=0
            
            # Store original version state
            local orig_major=$MAJOR
            local orig_minor=$MINOR
            local orig_patch=$PATCH
            
            print_debug "Analyzing ${#commits} brought commits for merge increment"
            
            # Analyze each commit
            while IFS= read -r commit_hash; do
                if [[ -n "$commit_hash" ]]; then
                    # Get commit message and author
                    commit_message=$(git log -1 --format="%B" "$commit_hash")
                    commit_author=$(git log -1 --format="%an <%ae>" "$commit_hash")
                    commit_short=$(git log -1 --format="%h" "$commit_hash")
                    
                    print_info "Testing commit $commit_short ($commit_message) for version increment"
                    
                    # Create a temporary copy of analyze_commit_message logic to test increment type
                    # without actually modifying the version numbers
                    local temp_increment_type="none"
                    
                    # Check for breaking changes (highest priority)
                    # Pattern for type!: or type(scope)!: format
                    local breaking_pattern="^[a-zA-Z]+(\([^)]+\))?!:[[:space:]]"
                    if [[ "$commit_message" =~ $breaking_pattern ]] || [[ "$commit_message" =~ BREAKING[[:space:]]+CHANGE: ]]; then
                        temp_increment_type="major"
                        print_info "  -> Breaking change detected (conventional commits ! or BREAKING CHANGE)"
                    # Check for explicit version change
                    elif [[ "$commit_message" =~ [Vv]ersion[[:space:]]+from[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]+to[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+) ]]; then
                        old_ver="${BASH_REMATCH[1]}"
                        new_ver="${BASH_REMATCH[2]}"
                        IFS='.' read -r old_major old_minor old_patch <<< "$old_ver"
                        IFS='.' read -r new_major new_minor new_patch <<< "$new_ver"
                        
                        if [ "$new_major" -gt "$old_major" ]; then
                            temp_increment_type="major"
                            print_info "  -> Explicit MAJOR version increment detected"
                        elif [ "$new_minor" -gt "$old_minor" ]; then
                            temp_increment_type="minor"
                            print_info "  -> Explicit MINOR version increment detected"
                        elif [ "$new_patch" -gt "$old_patch" ]; then
                            temp_increment_type="patch"
                            print_info "  -> Explicit PATCH version increment detected"
                        fi
                    # Check conventional format (excluding breaking changes which are handled above)
                    else
                        local feat_pattern="^feat(\([[:alnum:]-]+\))?:[[:space:]]"
                        local feat_breaking_pattern="^feat(\([[:alnum:]-]+\))?!:[[:space:]]"
                        local fix_pattern="^fix(\([[:alnum:]-]+\))?:[[:space:]]"
                        local fix_breaking_pattern="^fix(\([[:alnum:]-]+\))?!:[[:space:]]"
                        
                        if [[ "$commit_message" =~ $feat_pattern ]] && [[ ! "$commit_message" =~ $feat_breaking_pattern ]]; then
                            temp_increment_type="minor"
                            print_info "  -> Conventional feature commit detected"
                        elif [[ "$commit_message" =~ $fix_pattern ]] && [[ ! "$commit_message" =~ $fix_breaking_pattern ]]; then
                            temp_increment_type="patch"
                            print_info "  -> Conventional fix commit detected"
                        # Check legacy format
                        elif [[ "$commit_message" =~ feature: || "$commit_message" =~ ^feature ]]; then
                            temp_increment_type="minor"
                            print_info "  -> Legacy feature commit detected"
                        elif [[ "$commit_message" =~ (^|[[:space:],;:])fix ]]; then
                            temp_increment_type="patch"
                            print_info "  -> Legacy fix commit detected"
                        # Check GitHub references (lowest priority)
                        else
                            url_or_issue=$(echo "$commit_message" | grep -oE 'https://github\.com/[a-zA-Z0-9._-]+/[a-zA-Z0-9._-]+(/[a-zA-Z0-9._/-]+)?' | head -n 1)
                            if [[ -n "$url_or_issue" ]]; then
                                print_debug "  -> GitHub reference found, checking details"
                                details=$(fetch_github_details "$url_or_issue")
                                type=$(echo "$details" | jq -r '.type')
                                label=$(echo "$details" | jq -r '.label')
                                
                                if [[ "$type" == "pull_request" ]] || [[ "$type" == "issue" ]]; then
                                    if [[ "$label" == "feature" ]]; then
                                        temp_increment_type="minor"
                                        print_info "  -> GitHub feature reference detected"
                                    elif [[ "$label" == "bug" ]]; then
                                        temp_increment_type="patch"
                                        print_info "  -> GitHub bug reference detected"
                                    fi
                                fi
                            fi
                        fi
                    fi
                    
                    # Track the highest increment type
                    if [[ "$temp_increment_type" == "major" ]]; then
                        found_major=1
                        highest_increment="major"
                    elif [[ "$temp_increment_type" == "minor" ]] && [[ $found_major -eq 0 ]]; then
                        found_minor=1
                        highest_increment="minor"
                    elif [[ "$temp_increment_type" == "patch" ]] && [[ $found_major -eq 0 ]] && [[ $found_minor -eq 0 ]]; then
                        found_patch=1
                        highest_increment="patch"
                    fi
                fi
            done <<< "$commits"
            
            # Apply the highest increment found
            if [ $found_major -eq 1 ]; then
                MAJOR=$((orig_major + 1))
                MINOR=0
                PATCH=0
                version_incremented=1
                print_increment "Merge requires MAJOR version increment to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            elif [ $found_minor -eq 1 ]; then
                MINOR=$((orig_minor + 1))
                PATCH=0
                version_incremented=1
                print_increment "Merge requires MINOR version increment to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            elif [ $found_patch -eq 1 ]; then
                PATCH=$((orig_patch + 1))
                version_incremented=1
                print_increment "Merge requires PATCH version increment to $MAJOR.$MINOR.$PATCH"
                CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                return 0
            fi
            
            print_warning "No version bump rules matched for merge commits"
            return 1
        }

        print_header "FETCHING TAGS"
        # git fetch --tags

        # Get all tags that match the pattern up to the current commit
        MATCHING_TAGS=$(git tag -l "[0-9]*.[0-9]*.[0-9]*" --merged HEAD)
        print_debug "All tags: $(git tag -l)"
        print_debug "Matching tags up to current commit: $MATCHING_TAGS"

        # Check if there are any matching tags
        if [ -n "$MATCHING_TAGS" ]; then
            # Get the latest semver tag
            LATEST_TAG=$(echo "$MATCHING_TAGS" | sort -V | tail -1)
        fi
        print_info "Latest tag up to current commit: $LATEST_TAG"

        # Get commit hashes since the latest tag (or all commits if no tag exists)
        if [ -z "$LATEST_TAG" ]; then
            COMMIT_HASHES=$(git log --reverse --format="%H")
            LATEST_TAG="0.0.0"
            print_info "No existing version tags found up to current commit, starting from 0.0.0"
        else
            COMMIT_HASHES=$(git log --reverse ^$(git rev-parse ${LATEST_TAG}) HEAD --ancestry-path --first-parent --format="%H")
        fi

        # Check if current commit has a release tag
        HAS_RELEASE_TAG=$(git tag --points-at HEAD | grep -E '^release' || true)
        print_debug "Has release tag: $HAS_RELEASE_TAG"

        # Set target based on release tag presence
        if [ -n "$HAS_RELEASE_TAG" ]; then
            TARGET="release"
            echo "target=release" >> "$GITHUB_OUTPUT"
        else
            TARGET="dev"
            echo "target=dev" >> "$GITHUB_OUTPUT"
        fi
        print_info "Build target: $TARGET"

        # Skip if no commits found
        if [ -z "$COMMIT_HASHES" ]; then
            print_warning "No new commits found since $LATEST_TAG"
            
            # Check if current commit has a tag
            CURRENT_COMMIT_TAGS=$(git tag --points-at HEAD)
            if [ -n "$CURRENT_COMMIT_TAGS" ]; then
                print_info "Current commit already has tags: $CURRENT_COMMIT_TAGS"
                
                # Get current commit hash (first 8 chars)
                COMMIT_HASH=$(git rev-parse --short=8 HEAD)
                
                # Get timestamp from the commit
                TIMESTAMP=$(git log -1 --format="%cd" --date=format:'%y%m%d%H' HEAD)
                
                # Output version info
                VERSION_FULL="${LATEST_TAG}+${TIMESTAMP}-${COMMIT_HASH}"
                VERSION_RPM=$(echo "$VERSION_FULL" | tr '-' '.')
                VERSION_DEB=$VERSION_FULL
                
                echo "version=$LATEST_TAG" >> "$GITHUB_OUTPUT"
                echo "version_full=$VERSION_FULL" >> "$GITHUB_OUTPUT"
                echo "version_rpm=$VERSION_RPM" >> "$GITHUB_OUTPUT"
                echo "version_deb=$VERSION_DEB" >> "$GITHUB_OUTPUT"
                echo "version_updated=true" >> "$GITHUB_OUTPUT"
                
                print_result "$LATEST_TAG" "$VERSION_FULL" "$VERSION_RPM" "$VERSION_DEB" "$TARGET"
            else
                print_warning "No tags found on current commit, skipping version bump"
                echo "version_updated=false" >> "$GITHUB_OUTPUT"
            fi
            exit 0
        fi

        print_header "ANALYZING COMMITS FOR VERSION BUMP"

        # Parse current version
        IFS='.' read -r MAJOR MINOR PATCH <<< "$LATEST_TAG"
        print_debug "Current version components: MAJOR: $MAJOR, MINOR: $MINOR, PATCH: $PATCH"

        # Default ignore patterns
        IGNORE_PATTERNS="${IGNORE_PATTERNS}"
        print_debug "Ignore patterns: $IGNORE_PATTERNS"

        # Setup git for tag creation if in GitHub Actions
        if [ -n "$GITHUB_ACTOR" -a -n "$GITHUB_REPOSITORY" -a -n "$GITHUB_TOKEN" ]; then
            print_info "Configuring git for GitHub Actions"
            git config --global user.name "$GITHUB_ACTOR"
            git config --global user.email "$GITHUB_ACTOR@users.noreply.github.com"
            git remote set-url origin "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
        else
            print_warning "Not a GitHub Actions environment, skipping git configuration"
        fi

        # Variable to track the current version
        CURRENT_VERSION="$LATEST_TAG"

        # Initialize CREATED_TAGS array
        CREATED_TAGS=()

        # Temporary files for processing (similar to analyze2.sh)
        MERGE_COMMITS_FILE=$(mktemp)
        ALL_COMMITS_FILE=$(mktemp)
        PROCESSED_COMMITS_FILE=$(mktemp)

        # Cleanup function
        cleanup() {
            rm -f "$MERGE_COMMITS_FILE" "$ALL_COMMITS_FILE" "$PROCESSED_COMMITS_FILE"
        }
        trap cleanup EXIT

        # Step 1: Get all merge commits
        print_info "Step 1: Finding merge commits..."
        echo "$COMMIT_HASHES" | while IFS= read -r commit_hash; do
            if [[ -n "$commit_hash" ]] && is_merge_commit "$commit_hash"; then
                echo "$commit_hash"
            fi
        done > "$MERGE_COMMITS_FILE"
        
        MERGE_COUNT=$(wc -l < "$MERGE_COMMITS_FILE")
        print_info "Found $MERGE_COUNT merge commits"

        # Step 2: For each merge commit, find what commits it brings
        print_info "Step 2: Analyzing commits brought by each merge..."
        declare -A COMMIT_TO_MERGE_MAP
        declare -A MERGE_BROUGHT_MAP

        while IFS= read -r merge_commit; do
            if [[ -n "$merge_commit" ]]; then
                # Get the merge commit message to determine direction
                merge_message=$(git log -1 --format="%s" "$merge_commit")
                merge_short=$(git log -1 --format="%h" "$merge_commit")
                
                print_debug "Analyzing merge commit $merge_short: $merge_message"
                
                # Check if this is a merge bringing commits TO master (not FROM master)
                # Skip merges that are bringing master to another branch
                if [[ "$merge_message" =~ "into ".*[^[:space:]]$ ]] && [[ ! "$merge_message" =~ "into master" ]] && [[ ! "$merge_message" =~ "into main" ]]; then
                    # This is merging INTO a feature branch, not bringing commits to master
                    print_debug "Skipping merge $merge_short: merging into feature branch, not master/main"
                    continue
                fi
                
                # Additional check: make sure these commits are actually from a feature branch
                # If the merge message suggests it's bringing master to another branch, skip
                if [[ "$merge_message" =~ "Merge branch 'master'" ]] && [[ "$merge_message" =~ "into " ]]; then
                    print_debug "Skipping merge $merge_short: bringing master to another branch"
                    continue
                fi
                
                # Additional check: detect fast-forward merges or merges with no second parent
                # These don't bring commits from feature branches
                if ! git rev-parse "${merge_commit}^2" >/dev/null 2>&1; then
                    print_debug "Skipping merge $merge_short: no second parent (fast-forward or invalid merge)"
                    continue
                fi
                
                # Get the commits brought by this merge (excluding the merge commit itself)
                # This finds commits that are reachable from the merge commit but not from its first parent
                brought_commits=$(git log --reverse --format="%H" "${merge_commit}^1..${merge_commit}^2" 2>/dev/null || true)
                
                if [[ -n "$brought_commits" ]]; then
                    # Additional validation: check that we're currently on master/main or the merge was made to master/main
                    # Get the branch that contains this merge commit
                    containing_branches=$(git for-each-ref --contains="$merge_commit" --format='%(refname:short)' refs/remotes/origin/ 2>/dev/null || true)
                    
                    if [[ -n "$containing_branches" ]]; then
                        # Check if any of the containing branches is master or main
                        if ! echo "$containing_branches" | grep -wq -e 'origin/master' -e 'origin/main'; then
                            print_debug "Skipping merge $merge_short: not in master/main branch"
                            continue
                        fi
                    fi
                    
                    # Final check: make sure this isn't a merge bringing commits from master/main to a feature branch
                    # by checking if the second parent (feature branch) contains commits that are newer than the first parent
                    first_parent_date=$(git log -1 --format="%ct" "${merge_commit}^1" 2>/dev/null || echo "0")
                    second_parent_date=$(git log -1 --format="%ct" "${merge_commit}^2" 2>/dev/null || echo "0")
                    
                    # If the first parent (presumably master) is newer than the second parent (feature branch),
                    # this might be a merge bringing master to a feature branch
                    if [[ "$first_parent_date" -gt "$second_parent_date" ]] && [[ "$merge_message" =~ "Merge branch" ]]; then
                        # Double-check by looking at the branch names in the merge message
                        if [[ "$merge_message" =~ "Merge branch 'master'" ]] || [[ "$merge_message" =~ "Merge branch 'main'" ]]; then
                            print_debug "Skipping merge $merge_short: appears to be merging master/main into feature branch"
                            continue
                        fi
                    fi
                    
                    brought_count=$(echo "$brought_commits" | wc -l)
                    print_debug "Merge $merge_short brings $brought_count commits to master/main"
                    
                    # Store the mapping
                    MERGE_BROUGHT_MAP["$merge_commit"]="$brought_commits"
                    
                    # Create reverse mapping (commit -> merge that brought it)
                    while IFS= read -r brought_commit; do
                        if [[ -n "$brought_commit" ]]; then
                            # Only map if not already mapped (first occurrence wins)
                            if [[ -z "${COMMIT_TO_MERGE_MAP[$brought_commit]}" ]]; then
                                COMMIT_TO_MERGE_MAP["$brought_commit"]="$merge_commit"
                            fi
                        fi
                    done <<< "$brought_commits"
                else
                    print_debug "Merge $merge_short brings no commits (empty merge or fast-forward)"
                fi
            fi
        done < "$MERGE_COMMITS_FILE"

        # Step 3: Write all commits to file
        echo "$COMMIT_HASHES" > "$ALL_COMMITS_FILE"
        TOTAL_COMMITS=$(wc -l < "$ALL_COMMITS_FILE")
        print_info "Found $TOTAL_COMMITS total commits to analyze"

        # Variable to track if version was updated
        VERSION_UPDATED=false

        # Process each commit
        COMMIT_COUNT=0
        
        print_header "COMMIT ANALYSIS"
        
        while IFS= read -r commit; do
            if [[ -n "$commit" ]]; then
            COMMIT_COUNT=$((COMMIT_COUNT + 1))
                
                # Skip if already processed
                if grep -q "^$commit$" "$PROCESSED_COMMITS_FILE" 2>/dev/null; then
                    continue
                fi
                
                # Get commit info
                commit_info=$(get_commit_info "$commit")
                IFS='|' read -r subject author email date <<< "$commit_info"
                full_author="$author <$email>"
                
                # Get commit hash (short)
                commit_short=$(git log -1 --format="%h" "$commit")
            
            # Get changed files for this commit
                changed_files=$(git diff-tree --no-commit-id --name-only -r "$commit")
                
                # Check commit type and process accordingly
                if is_merge_commit "$commit"; then
                    # This is a merge commit
                    safe_echo "[$date] ${MAGENTA}MERGE:${NC} $subject - $author"
                    safe_echo "  ${CYAN}Commit: $commit_short${NC}"
                    
                    # Variables to track version increment for this merge
                    OLD_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                    merge_incremented=false
                    
                    # Show and analyze brought commits
                    if [[ -n "${MERGE_BROUGHT_MAP[$commit]}" ]]; then
                        safe_echo "  ${YELLOW}Brought commits:${NC}"
                        
                        # Analyze all brought commits for version increment
                        if analyze_multiple_commits "${MERGE_BROUGHT_MAP[$commit]}"; then
                            merge_incremented=true
                            create_version_tag "$CURRENT_VERSION" "$commit"
                            VERSION_UPDATED=true
                        fi
                        
                        # Display brought commits
                        while IFS= read -r brought_commit; do
                            if [[ -n "$brought_commit" ]]; then
                                brought_info=$(get_commit_info "$brought_commit")
                                IFS='|' read -r brought_subject brought_author brought_email brought_date <<< "$brought_info"
                                brought_short=$(git log -1 --format="%h" "$brought_commit")
                                safe_echo "    [$brought_date] ${CYAN}BROUGHT:${NC} $brought_subject - $brought_author"
                                safe_echo "      ${CYAN}Commit: $brought_short${NC}"
                                
                                # Mark as processed
                                echo "$brought_commit" >> "$PROCESSED_COMMITS_FILE"
                            fi
                        done <<< "${MERGE_BROUGHT_MAP[$commit]}"
                    fi
                    
                    # Show increment info for merge
                    if [ "$merge_incremented" = true ]; then
                        safe_echo "  ${GREEN}INCREMENT:${NC} $OLD_VERSION → $CURRENT_VERSION"
                        safe_echo "  ${GREEN}TAG:${NC} $CURRENT_VERSION"
                    else
                        safe_echo "  ${YELLOW}INCREMENT:${NC} None"
                        safe_echo "  ${YELLOW}TAG:${NC} None"
                    fi
                    
                    # Mark merge commit as processed
                    echo "$commit" >> "$PROCESSED_COMMITS_FILE"
                    
                elif [[ -n "${COMMIT_TO_MERGE_MAP[$commit]}" ]]; then
                    # This commit was brought by a merge - skip it here as it will be shown with the merge
                    continue
                    
                else
                    # This is a simple commit
                    safe_echo "[$date] ${GREEN}SIMPLE:${NC} $subject - $author"
                    safe_echo "  ${CYAN}Commit: $commit_short${NC}"
            
            # Display changed files if in debug mode
            print_file_list "$changed_files"
            
                    # Variables to track version increment
            OLD_VERSION="${MAJOR}.${MINOR}.${PATCH}"
                    commit_incremented=false

            # Skip if no files were changed (e.g. merge commits)
            if [ -z "$changed_files" ]; then
                        safe_echo "  ${YELLOW}INCREMENT:${NC} None - No files changed"
                        safe_echo "  ${YELLOW}TAG:${NC} None"
                        echo "$commit" >> "$PROCESSED_COMMITS_FILE"
                        safe_echo ""
                continue
            fi

            # Skip version bump if commit is not in master/main branch
                    if ! git for-each-ref --contains="$commit" --format='%(refname:short)' refs/remotes/origin/ | grep -wq -e 'origin/master' -e 'origin/main'; then
                        safe_echo "  ${YELLOW}INCREMENT:${NC} None - Not in master/main branch"
                        safe_echo "  ${YELLOW}TAG:${NC} None"
                        echo "$commit" >> "$PROCESSED_COMMITS_FILE"
                        safe_echo ""
                continue
            fi
            
            # Check if commit touches only ignored files
            all_files_ignored=true
            if [ -z "$IGNORE_PATTERNS" ]; then
                # If IGNORE_PATTERNS is empty, don't ignore any files
                all_files_ignored=false
            else
                while IFS= read -r file; do
                    if [[ ! "$file" =~ ($IGNORE_PATTERNS) ]]; then
                        all_files_ignored=false
                        break
                    fi
                done <<< "$changed_files"
            fi
            
            # Skip version bump if all changed files are ignored
            if [ "$all_files_ignored" = true ]; then
                        safe_echo "  ${YELLOW}INCREMENT:${NC} None - All files ignored"
                        safe_echo "  ${YELLOW}TAG:${NC} None"
                        echo "$commit" >> "$PROCESSED_COMMITS_FILE"
                        safe_echo ""
                continue
            fi
            
            # Analyze commit message for version bump
                    if analyze_commit_message "$(git log -1 --format="%B" $commit)" "$full_author"; then
                        create_version_tag "$CURRENT_VERSION" "$commit"
                VERSION_UPDATED=true
                        commit_incremented=true
                    fi
                    
                    # Show increment info
                    if [ "$commit_incremented" = true ]; then
                        safe_echo "  ${GREEN}INCREMENT:${NC} $OLD_VERSION → $CURRENT_VERSION"
                        safe_echo "  ${GREEN}TAG:${NC} $CURRENT_VERSION"
                    else
                        safe_echo "  ${YELLOW}INCREMENT:${NC} None"
                        safe_echo "  ${YELLOW}TAG:${NC} None"
                    fi
                    
                    # Mark as processed
                    echo "$commit" >> "$PROCESSED_COMMITS_FILE"
                fi
                
                safe_echo ""
            fi
        done < "$ALL_COMMITS_FILE"

        print_header "GENERATING VERSION INFO"

        # Use the current version for output
        NEW_TAG="$CURRENT_VERSION"
        print_info "Final version: $NEW_TAG (started from $LATEST_TAG)"

        # Get current branch name
        if [ -n "$GITHUB_HEAD_REF" ]; then
            # In a pull request, use the source branch name
            print_debug "GITHUB_HEAD_REF: $GITHUB_HEAD_REF"
            BRANCH_NAME="$GITHUB_HEAD_REF"
        elif [[ "$GITHUB_REF" =~ ^refs/tags/ ]]; then
            # If triggered by a tag, try to find the branch that contains this tag
            print_debug "Triggered by tag: $GITHUB_REF"
            TAG_NAME=${GITHUB_REF#refs/tags/}
            # Get the commit hash that the tag points to
            TAG_COMMIT=$(git rev-parse $TAG_NAME)
            # Find which branch contains this commit
            BRANCH_NAME=$(git for-each-ref --contains=$TAG_COMMIT --format='%(refname:short)' refs/remotes/origin/ | egrep "master|main" | sed 's#^origin/##' | head -n 1)
            if [ -z "$BRANCH_NAME" ]; then
                # If not found in master/main, take the first branch that contains it
                BRANCH_NAME=$(git for-each-ref --contains=$TAG_COMMIT --format='%(refname:short)' refs/remotes/origin/ | head -n 1)
            fi
            print_debug "Found branch for tag: $BRANCH_NAME"
        else
            # In a normal push, use the current branch name
            print_debug "normal push"
            BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
        fi
        print_debug "Current branch: $BRANCH_NAME"

        # Semver does not allow some symbols that branch may have
        VERSION_SUFFIX=$(echo "$BRANCH_NAME" | sed 's/[^a-zA-Z0-9-]//g')

        # Get current commit hash (first 8 chars)
        COMMIT_HASH=$(git rev-parse --short=8 HEAD)

        # Get timestamp from the commit in YYMMDDHH format
        TIMESTAMP=$(git log -1 --format="%cd" --date=format:'%y%m%d%H' HEAD)
            
        # Generate full version string
        if [ -z "$HAS_RELEASE_TAG" ]; then
            if [[ "$BRANCH_NAME" == "master" || "$BRANCH_NAME" == "main" ]]; then
                VERSION_FULL="${NEW_TAG}+${TIMESTAMP}-${COMMIT_HASH}-dev"
            else
                VERSION_FULL="${NEW_TAG}+${TIMESTAMP}-${COMMIT_HASH}-${VERSION_SUFFIX}"
            fi
        else
            VERSION_FULL="${NEW_TAG}+${TIMESTAMP}-${COMMIT_HASH}"
        fi

        VERSION_RPM=$(echo "$VERSION_FULL" | tr '-' '.')
        VERSION_DEB=$(echo "$VERSION_FULL")

        # Always write version info to output
        echo "version=$NEW_TAG" >> "$GITHUB_OUTPUT"
        echo "version_full=$VERSION_FULL" >> "$GITHUB_OUTPUT"
        echo "version_rpm=$VERSION_RPM" >> "$GITHUB_OUTPUT"
        echo "version_deb=$VERSION_DEB" >> "$GITHUB_OUTPUT"
        echo "version_updated=$VERSION_UPDATED" >> "$GITHUB_OUTPUT"

        # Print final results
        print_result "$NEW_TAG" "$VERSION_FULL" "$VERSION_RPM" "$VERSION_DEB" "$TARGET"

        # Push all tags at the end for better efficiency
        push_all_tags