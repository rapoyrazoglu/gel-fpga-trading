# Security

This is a research and teaching project. It has never been connected to a live
venue and holds no customer data.

If you find a security problem, please do not open a public issue. Use GitHub's
private vulnerability reporting on this repository ("Security" tab, "Report a
vulnerability"). You should get a reply within a week.

Things worth knowing before you report:

- The HMAC key used in tests and in the on-silicon driver is a fixed test key.
  It is not a secret and must be replaced in any real deployment.
- The parameter-block HMAC check exists in RTL and passes in simulation, but
  the silicon build leaves it out. See the README's scope section.
