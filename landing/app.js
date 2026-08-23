const requestedTheme = new URLSearchParams(window.location.search).get("theme");
if (requestedTheme === "light" || requestedTheme === "dark") {
  document.documentElement.dataset.theme = requestedTheme;
}

const navToggle = document.querySelector(".nav-toggle");
const navLinks = document.querySelector(".nav-links");

function closeMenu() {
  if (!navToggle || !navLinks) return;
  navToggle.setAttribute("aria-expanded", "false");
  navToggle.textContent = "Menu";
  navLinks.classList.remove("is-open");
  document.body.classList.remove("nav-open");
}

if (navToggle && navLinks) {
  navToggle.addEventListener("click", () => {
    const willOpen = navToggle.getAttribute("aria-expanded") !== "true";
    navToggle.setAttribute("aria-expanded", String(willOpen));
    navToggle.textContent = willOpen ? "Close" : "Menu";
    navLinks.classList.toggle("is-open", willOpen);
    document.body.classList.toggle("nav-open", willOpen);
  });

  navLinks.querySelectorAll("a").forEach((link) => {
    link.addEventListener("click", closeMenu);
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") closeMenu();
  });
}

const captionSets = {
  hinglish: {
    generic: "मैंने कहा के audience का trust build करो",
    crux: "Maine kaha ke audience ka trust build karo.",
    preview: "Maine kaha ke <strong>audience</strong> ka trust build karo."
  },
  hindi: {
    generic: "मैंने कहा ki audience का भरोसा बनाओ",
    crux: "Maine kaha ki audience ka bharosa banao.",
    preview: "Maine kaha ki audience ka <strong>bharosa</strong> banao."
  },
  urdu: {
    generic: "میں نے کہا کے audience کا trust build کرو",
    crux: "Main ne kaha ke audience ka aitmaad qaim karo.",
    preview: "Main ne kaha ke <strong>aitmaad</strong> qaim karo."
  }
};

const genericCaption = document.querySelector("#generic-caption");
const cruxCaption = document.querySelector("#crux-caption");
const videoCaption = document.querySelector("#video-caption");
const demoButtons = document.querySelectorAll(".demo-button[data-caption]");

demoButtons.forEach((button) => {
  button.addEventListener("click", () => {
    const set = captionSets[button.dataset.caption];
    if (!set || !genericCaption || !cruxCaption || !videoCaption) return;

    genericCaption.textContent = set.generic;
    cruxCaption.textContent = set.crux;
    videoCaption.innerHTML = set.preview;

    demoButtons.forEach((item) => {
      const isActive = item === button;
      item.classList.toggle("is-active", isActive);
      item.setAttribute("aria-pressed", String(isActive));
    });
  });
});

const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
const revealItems = document.querySelectorAll(".reveal");

if (!reduceMotion && "IntersectionObserver" in window) {
  document.body.classList.add("motion-ready");
  const revealObserver = new IntersectionObserver((entries, observer) => {
    entries.forEach((entry) => {
      if (!entry.isIntersecting) return;
      entry.target.classList.add("is-visible");
      observer.unobserve(entry.target);
    });
  }, {
    threshold: 0.14,
    rootMargin: "0px 0px -6% 0px"
  });

  revealItems.forEach((item) => revealObserver.observe(item));
} else {
  revealItems.forEach((item) => item.classList.add("is-visible"));
}
