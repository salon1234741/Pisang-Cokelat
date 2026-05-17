const header = document.querySelector("[data-header]");
const menuToggle = document.querySelector("[data-menu-toggle]");
const mobileNav = document.querySelector("[data-mobile-nav]");
const navLinks = document.querySelectorAll(".nav-link");
const sections = document.querySelectorAll("main section[id]");
const revealItems = document.querySelectorAll(".reveal");
const filterButtons = document.querySelectorAll("[data-filter]");
const productCards = document.querySelectorAll("[data-category]");
const faqQuestions = document.querySelectorAll(".faq-question");
const contactForm = document.querySelector("[data-contact-form]");

function setHeaderState() {
  header.classList.toggle("scrolled", window.scrollY > 16);
}

function setActiveNav() {
  const current = [...sections].find((section) => {
    const rect = section.getBoundingClientRect();
    return rect.top <= 120 && rect.bottom >= 120;
  });

  if (!current) return;

  navLinks.forEach((link) => {
    link.classList.toggle("active", link.getAttribute("href") === `#${current.id}`);
  });
}

function setupRevealAnimation() {
  if (!("IntersectionObserver" in window)) {
    revealItems.forEach((item) => item.classList.add("in-view"));
    return;
  }

  const observer = new IntersectionObserver(
    (entries) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          entry.target.classList.add("in-view");
          observer.unobserve(entry.target);
        }
      });
    },
    { threshold: 0.18 }
  );

  revealItems.forEach((item) => observer.observe(item));
}

function setupMenuToggle() {
  menuToggle?.addEventListener("click", () => {
    const isOpen = mobileNav.classList.toggle("open");
    menuToggle.setAttribute("aria-expanded", String(isOpen));
    menuToggle.querySelector(".material-symbols-outlined").textContent = isOpen ? "close" : "menu";
  });

  mobileNav?.addEventListener("click", (event) => {
    if (event.target.matches("a")) {
      mobileNav.classList.remove("open");
      menuToggle.setAttribute("aria-expanded", "false");
      menuToggle.querySelector(".material-symbols-outlined").textContent = "menu";
    }
  });
}

function setupMenuFilter() {
  filterButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const filter = button.dataset.filter;

      filterButtons.forEach((item) => item.classList.remove("active"));
      button.classList.add("active");

      productCards.forEach((card) => {
        const categories = card.dataset.category.split(" ");
        const shouldShow = filter === "all" || categories.includes(filter);
        card.classList.toggle("hidden", !shouldShow);
      });
    });
  });
}

function setupFaq() {
  faqQuestions.forEach((question) => {
    question.addEventListener("click", () => {
      const item = question.closest(".faq-item");
      const isOpen = item.classList.toggle("open");
      question.setAttribute("aria-expanded", String(isOpen));
    });
  });
}

function setupCountdown() {
  const hoursEl = document.querySelector("[data-hours]");
  const minutesEl = document.querySelector("[data-minutes]");
  const secondsEl = document.querySelector("[data-seconds]");
  if (!hoursEl || !minutesEl || !secondsEl) return;

  let remaining = (5 * 60 * 60) + (42 * 60) + 18;

  setInterval(() => {
    remaining = remaining > 0 ? remaining - 1 : (6 * 60 * 60);

    const hours = Math.floor(remaining / 3600);
    const minutes = Math.floor((remaining % 3600) / 60);
    const seconds = remaining % 60;

    hoursEl.textContent = String(hours).padStart(2, "0");
    minutesEl.textContent = String(minutes).padStart(2, "0");
    secondsEl.textContent = String(seconds).padStart(2, "0");
  }, 1000);
}

function setupOrderButtons() {
  document.querySelectorAll(".order-button").forEach((button) => {
    button.addEventListener("click", () => {
      const product = button.dataset.product;
      const message = encodeURIComponent(`Halo, saya mau pesan ${product}.`);
      window.open(`https://wa.me/6281217291743?text=${message}`, "_blank", "noopener");
    });
  });
}

function setupContactForm() {
  contactForm?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formData = new FormData(contactForm);
    const name = formData.get("name");
    const contact = formData.get("contact");
    const message = formData.get("message");
    const whatsappMessage = encodeURIComponent(
      `Halo, saya ${name}. Kontak saya: ${contact}. Pesan: ${message}`
    );

    window.open(`https://wa.me/6281217291743?text=${whatsappMessage}`, "_blank", "noopener");
    contactForm.reset();
  });
}

window.addEventListener("scroll", () => {
  setHeaderState();
  setActiveNav();
}, { passive: true });

setHeaderState();
setActiveNav();
setupRevealAnimation();
setupMenuToggle();
setupMenuFilter();
setupFaq();
setupCountdown();
setupOrderButtons();
setupContactForm();
