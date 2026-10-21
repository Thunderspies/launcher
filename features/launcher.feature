Feature: Install and maintain publisher releases
  Players install a release selected by their configured channel and can
  update or repair it while retaining unrelated personal files.

  Background:
    Given a publisher repository is served over HTTP

  Scenario: Install a release using its public metadata and original objects
    When the launcher updates the installation
    Then release r1 is installed and verifies
    And the game contains the bytes from r1

  Scenario: Keep the published launch command with the installed release
    Given the release supplies a game command without local game settings
    When the launcher updates the installation
    And the installation is reopened after the channel moves
    Then the installed release retains its published game command

  Scenario: Update to another release while retaining personal settings
    Given release r1 is installed
    And the installation has unrelated personal settings
    And the channel is pinned to r2
    When the launcher updates the installation
    Then release r2 is installed and verifies
    And the game contains the bytes from r2
    And the personal settings are preserved

  Scenario: Repair the saved release independently of the channel
    Given release r1 is installed
    And the installed game has been damaged
    And the channel is pinned to r2
    When the launcher repairs the installation
    Then release r1 is installed and verifies
    And the game contains the bytes from r1

  Scenario: Reject malformed metadata and preserve the installed game
    Given release r1 is installed
    And the channel metadata is malformed
    When the launcher updates the installation
    Then the update reports invalid metadata
    And the game contains the bytes from r1

  Scenario: An empty release retains a valid archive and personal settings
    Given release r1 is installed
    And the installation has unrelated personal settings
    And the channel is pinned to empty
    When the launcher updates the installation
    Then release empty is installed and verifies
    And the managed loose files are removed
    And the personal settings are preserved

  Scenario: Install from a private S3-compatible repository
    Given the repository requires S3 credentials
    When the launcher updates the installation
    Then release r1 is installed and verifies
    And the game contains the bytes from r1

  Scenario: Repair from a private S3-compatible repository after reopening
    Given the repository requires S3 credentials
    And release r1 is installed
    And the installed game has been damaged
    And the installation is reopened with fresh S3 credentials
    When the launcher repairs the installation
    Then release r1 is installed and verifies
    And the game contains the bytes from r1

  Scenario: Reject invalid credentials and preserve the installed game
    Given the repository requires S3 credentials
    And release r1 is installed
    And the S3 credentials are invalid
    When the launcher updates the installation
    Then the update reports an authentication failure
    And the game contains the bytes from r1

  Scenario: Preserve publisher compressed object bytes in the game archive
    Given the channel is pinned to compressed
    When the launcher updates the installation
    Then release compressed is installed and verifies
    And the archive contains the exact publisher stored objects

  Scenario: Detect cache corruption only during explicit verification
    Given release r1 is installed
    And an accepted cache object has same-size corruption
    When the launcher verifies the installation
    Then verification reports integrity failure and discards the cache object
    When the launcher repairs the installation
    Then release r1 is installed and verifies

  Scenario: Preserve cached headers and restore them during repair
    Given the channel is pinned to headers
    When the launcher updates the installation
    Then release headers is installed and verifies
    And the archive retains the published binary cached headers
    Given the archive cached headers are missing
    When the launcher repairs the installation
    Then release headers is installed and verifies
    And the archive retains the published binary cached headers

  Scenario: Resolve descriptors through ordered file repositories
    Given file descriptors use a separate repository after an empty mirror
    When the launcher updates the installation
    Then release r1 is installed and verifies
    And only the selected file repository supplies the payloads

  Scenario: Install public manifests with independently private files
    Given the public manifest uses a private file repository
    When the launcher updates the installation
    Then release r1 is installed and verifies
    And the game contains the bytes from r1


  Scenario: Preserve header objects and restore archive headers during repair
    Given the channel is pinned to headerobjects
    When the launcher updates the installation
    Then release headerobjects is installed and verifies
    And the archive retains the published binary cached headers
    Given the archive cached headers are missing
    When the launcher repairs the installation
    Then release headerobjects is installed and verifies
    And the archive retains the published binary cached headers

  Scenario: Restore header objects offline from a valid installed archive
    Given the channel is pinned to headerobjects
    When the launcher updates the installation
    Then release headerobjects is installed and verifies
    Given the header objects are missing and file repositories are offline
    When the launcher repairs the installation
    Then release headerobjects is installed and verifies
    And the archive retains the published binary cached headers

  Scenario: Install multiple archives and apply a release with no groups
    Given the release contains two HOGG groups
    When the launcher updates the installation
    Then release multi is installed and verifies
    And both managed archives are installed
    Given the installation has unrelated personal settings
    And the release is recommitted without any file groups
    When the launcher updates the installation
    Then release multi is installed and verifies
    And all managed archives and loose files are removed
    And the personal settings are preserved

  Scenario: Keep physical files when the published launch command changes
    Given release r1 is installed
    And the installed physical files are marked for preservation
    And the release is recommitted with a different launch argument
    When the launcher updates the installation
    Then release r1 is installed and verifies
    And the installed physical files are preserved without cache population

  Scenario: Patch changed archive entries without rewriting retained payloads
    Given release r1 is installed
    And the channel is pinned to r2
    When the launcher updates the installation
    Then release r2 is installed and verifies
    And the archive contains the exact publisher stored objects

  Scenario: Launch local changes without auditing unrelated managed content
    Given release r1 is installed
    And the game uses a controlled runner
    And managed files contain local changes
    When the launcher starts the saved game
    Then the controlled game completes successfully
    When the launcher verifies the installation
    Then verification reports local content changes
