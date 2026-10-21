Feature: Native runner regression fixtures

  @passing
  Scenario: Native assertion succeeds
    Given a native fixture passes

  @failing
  Scenario: Native assertion fails
    Given a native fixture fails

  @pending
  Scenario: Native step is pending
    Given a native fixture is pending

  @undefined
  Scenario: Native step is undefined
    Given a native fixture has no definition
